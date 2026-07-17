// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Bluetooth NES Advantage firmware entry point.
//
// Default build (env `wroom32`): the gameplay firmware. It ties together dual-mode transport select
// (Classic Switch Pro / BLE), player select, button profiles, directional modes, hold-button
// gestures, LEDs, battery, and the deep-sleep / ULP-wake state machine, behind the transport-neutral
// bt:: API.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <cstring>
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "settings.hpp"
#include "battery.hpp"
#include "power.hpp"
#include "nes_controller.hpp"
#include "bt_transport.hpp"
#include "bt_config.hpp"
#include "console.hpp"
#include "app_control.hpp"

static const char* TAG = "app";

// --- Timings (ms unless noted) -----------------------------------------------------------------
static constexpr int64_t kConnIdleTimeoutMs = 90'000;    // unconnected this long -> sleep
static constexpr int64_t kIdleTimeoutMs     = 300'000;   // no button activity this long -> sleep
static constexpr int64_t kGestureHoldMs     = 5'000;     // all hold-combos: 5 s
static constexpr int64_t kBatteryPollMs     = 1'000;

// --- Persisted across deep sleep (survives the reset that deep sleep performs) ------------------
RTC_DATA_ATTR static bool s_sleeping = false;
// One-shot request set by the A+B+Select gesture: the next boot enters BLE config/OTA mode, then a
// plain reboot (or an applied setting) returns to normal gameplay. This must survive esp_restart(),
// so it uses RTC_NOINIT_ATTR (RTC_DATA_ATTR is RE-INITIALIZED on a software reset, it only persists
// across deep-sleep wake). It's uninitialized on power-on, so we only honor it after a software
// reset (esp_restart) and clear it once read.
RTC_NOINIT_ATTR static uint32_t s_config_magic;
static constexpr uint32_t kConfigMagic = 0xC0FF1900;

// --- Globals -----------------------------------------------------------------------------------
static NESController* nes = nullptr;
static uint8_t  s_player  = 0;
static uint8_t  s_profile = 0;
static uint8_t  s_dirmode = 0;
static int64_t  s_last_connected_ms = 0;
static int64_t  s_last_battery_ms   = 0;
static bool     s_start_release_required = false;   // set on Start-wake; blocks instant re-sleep
static bool     s_led_auto = true;                  // false = console owns the LEDs (see app::set_led_auto)
static bool     s_sleep_requested = false;          // console `sleep`: enter deep sleep from the loop
static bool     s_sleep_inhibit = false;            // bench: suppress idle/disconnect auto-sleep
static uint8_t  s_raw_p1 = 0xFF, s_raw_p2 = 0xFF;   // last raw controller sample (for `diag`)
static bt::NesInput s_last_sent;                    // last snapshot handed to the transport
static bool     s_have_sent = false;                // false = force a send (nothing sent, or report zeroed)

static inline int64_t now_ms() { return esp_timer_get_time() / 1000; }

// --- LEDs (active-low) -------------------------------------------------------------------------
static void led_init() {
    gpio_config_t io = {};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << LED_RED) | (1ULL << LED_GREEN) | (1ULL << LED_BLUE);
    gpio_config(&io);
    gpio_set_level((gpio_num_t)LED_RED, 1);
    gpio_set_level((gpio_num_t)LED_GREEN, 1);
    gpio_set_level((gpio_num_t)LED_BLUE, 1);
}
static inline void led_set(int pin, bool on) { gpio_set_level((gpio_num_t)pin, on ? 0 : 1); }

// Blink an LED `count` times (blocking) to confirm a gesture selection.
static void led_blink(int pin, uint8_t count) {
    led_set(pin, false);
    vTaskDelay(pdMS_TO_TICKS(400));
    for (uint8_t i = 0; i < count; i++) {
        led_set(pin, true);  vTaskDelay(pdMS_TO_TICKS(250));
        led_set(pin, false); vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// --- Sleep -------------------------------------------------------------------------------------
static void enter_sleep() {
    ESP_LOGI(TAG, "entering deep sleep (ULP polls the controller; press Start to wake)");
    led_set(LED_RED, false); led_set(LED_GREEN, false); led_set(LED_BLUE, false);

    // Start both sleeps AND wakes the device, so the ULP baseline must be captured with Start
    // released. If we arm the ULP while the sleep-hold is still down, that held state reads as a
    // change on the first post-sleep poll and wakes us right back up. Wait for Start to be released
    // (debounced) before arming, with a safety cap so a stuck/garbage read can't hang here forever.
    // (When called from the idle/disconnect timeouts Start isn't held, so this returns immediately.)
    int64_t deadline = now_ms() + 3000;
    int released = 0;
    while (now_ms() < deadline) {
        nes->read();
        released = nes->getButtonState(s_player, NESController::BUTTON_START) ? 0 : released + 1;
        if (released >= 5) break;       // ~50ms continuously released
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (released < 5) ESP_LOGW(TAG, "Start still held at sleep; arming anyway");

    s_sleeping = true;
    power::ulp_load();                 // arm the ULP 4021 poll (wake-on-button-change)
    vTaskDelay(pdMS_TO_TICKS(120));    // let the serial log flush
    power::deep_sleep();               // does not return; SoC reboots on the next ULP wake
}

// On boot after a deep sleep, decide whether to wake fully (Start pressed) or go back to sleep.
// Returns true if we should continue with a full boot.
static bool handle_wake() {
    if (!s_sleeping) return true;      // cold boot / normal start
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool start_held = power::button_pressed_in_last_read(NESController::BUTTON_START);
    if (cause == ESP_SLEEP_WAKEUP_ULP && start_held) {
        ESP_LOGI(TAG, "ULP wake + Start held -> powering on");
        s_sleeping = false;
        s_start_release_required = true;     // don't let the same hold immediately re-sleep
        power::release_after_wake();         // hand the NES pins back to the normal gpio driver
        return true;
    }
    ESP_LOGI(TAG, "wake without Start (cause=%d) -> back to sleep", cause);
    power::ulp_load();
    vTaskDelay(pdMS_TO_TICKS(120));
    power::deep_sleep();
    return false;                      // unreachable
}

// --- Player select -----------------------------------------------------------------------------
static void chords_reset();            // defined with the chord layer below

static void on_player_change(uint8_t newPlayer) {
    bt::clear_input();                 // zero the previous player's report before switching
    s_player = newPlayer;
    chords_reset();                    // the shift latch and any pending Minus pulse were the old player's
    s_have_sent = false;               // clear_input zeroed the report; don't let a compare skip the resend
    ESP_LOGI(TAG, "player select -> P%d", s_player + 1);
}

// --- Profiles / directional modes --------------------------------------------------------------
static void cycle_profile() {
    s_profile = (s_profile + 1) % bt::num_profiles();
    settings::set_profile(s_profile);
    ESP_LOGI(TAG, "profile -> %u (%s)", s_profile + 1, bt::profile_name(s_profile));
    led_blink(LED_RED, s_profile + 1);
}
static void cycle_directional_mode() {
    s_dirmode = (s_dirmode + 1) % bt::num_directional_modes();
    settings::set_directional_mode(s_dirmode);
    ESP_LOGI(TAG, "directional mode -> %u (%s)", s_dirmode + 1, bt::directional_mode_name(s_dirmode));
    led_blink(LED_RED, s_dirmode + 1);
}
static void enter_config_mode() {
    ESP_LOGW(TAG, "entering BLE config/OTA mode; rebooting");
    led_blink(LED_BLUE, 3);
    s_config_magic = kConfigMagic;   // survives esp_restart (RTC_NOINIT); consumed on next boot
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
static void toggle_transport() {
    uint8_t next = (settings::transport() + 1) % bt::TRANSPORT_COUNT;
    settings::set_transport(next);
    ESP_LOGW(TAG, "transport -> %s; rebooting", bt::transport_name((bt::Transport)next));
    led_blink(LED_BLUE, next + 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// --- Chords: Select as a shift key -------------------------------------------------------------
// Buttons the Switch has and the NES doesn't, reachable mid-game. Held on its own Select is still
// Minus; pressed with another button it shifts that button to a Pro button:
//
//   Select + Start -> Home            Select + Left  -> ZL
//   Select + Up    -> ZL+ZR           Select + Right -> ZR
//   Select + Down  -> Capture
//
// Chord output is held for as long as the chord is (so Select+Down holds Capture to record a clip,
// and ZL/ZR work in games that want them held), and a chord suppresses the normal role of every
// button it consumes.
//
// A/B are deliberately not chord members: the Turbo dials pulse those lines inside the Advantage's
// own hardware, so any A/B chord would misfire with turbo on, which is exactly when it'd be used.
//
// Three rules keep this from firing during play:
//
//  - Only a button that wasn't already held when Select landed shifts. Holding Right and tapping
//    Select has to stay Right + Minus, not ZR, so s_shift_passthru latches the ones that were.
//  - Minus is withheld while Select is down and emitted as a pulse on release if no chord formed.
//    Deferral is the only thing that works here: "hold Select, then push the stick" puts the two
//    presses hundreds of ms apart, so no simultaneity window is wide enough to catch it.
//  - A chord blocks the 5 s hold gestures until Select is released. Select+direction is otherwise
//    G_FORGET (see detect_gesture), which would re-pair the stick if a chord were held too long.
//    Select+Start is left alone, so holding it 5 s still toggles transport; the Switch sees a Home
//    press first, which is harmless when you're leaving that host anyway.
//
// The slow-motion switch pulses Start in hardware, the way the Turbo dials pulse A/B, so it has to
// be off to use Select+Start. It already pulses Plus into the game, so it isn't usable in play
// regardless. Documented in docs/MANUAL.md rather than worked around.
static constexpr int64_t kMinusPulseMs = 80;   // >= 10 Switch polls (8 ms apart); still reads as a tap

enum ChordBit : uint8_t {   // chord members, in NES poll order after Select
    CH_START = 0x01, CH_UP = 0x02, CH_DOWN = 0x04, CH_LEFT = 0x08, CH_RIGHT = 0x10,
};

static bool    s_shift_armed = false;       // Select is down
static uint8_t s_shift_passthru = 0;        // members already held when Select landed; not chordable
static bool    s_chord_fired = false;       // a chord formed during this Select press
static uint8_t s_chord_active = 0;          // members currently shifted (for the log edge)
static int64_t s_minus_pulse_until = 0;

// Raw member state one poll ago. Deliberately NOT part of chords_reset(): this is button history,
// not chord state. Zeroing it when Select is released would make the next Select press see an
// already-held direction as freshly pressed and chord it, which is the exact misfire the passthru
// latch exists to stop (hold Right, tap Select, tap Select again -> phantom ZR).
static uint8_t s_prev_members = 0;

static void chords_reset() {
    s_shift_armed = false;
    s_shift_passthru = 0;
    s_chord_fired = false;
    s_chord_active = 0;
    s_minus_pulse_until = 0;
}

static const char* chord_name(uint8_t active) {
    switch (active) {
    case CH_START: return "Home";
    case CH_UP:    return "ZL+ZR";
    case CH_DOWN:  return "Capture";
    case CH_LEFT:  return "ZL";
    case CH_RIGHT: return "ZR";
    default:       return "multiple";
    }
}

// Resolve the shift layer in place. Must run every poll, not just on a button change: the Minus
// pulse expires on a timer.
static void apply_chords(bt::NesInput& in) {
    const uint8_t members = (in.start ? CH_START : 0) | (in.up    ? CH_UP    : 0) |
                            (in.down  ? CH_DOWN  : 0) | (in.left  ? CH_LEFT  : 0) |
                            (in.right ? CH_RIGHT : 0);

    if (in.select && !s_shift_armed) {           // Select landed: latch what it can't claim
        s_shift_armed = true;
        // Only what was ALREADY down a poll ago is passthru. Comparing against `members` instead
        // would make a member that lands in the same 2 ms poll as Select look pre-existing, so
        // slamming Select+Up together (the natural way to hit a chord) would silently miss.
        s_shift_passthru = s_prev_members & members;
        s_chord_fired = false;
        s_chord_active = 0;
    } else if (!in.select && s_shift_armed) {    // Select released
        bool plain_select = !s_chord_fired;
        chords_reset();
        if (plain_select) s_minus_pulse_until = now_ms() + kMinusPulseMs;
    }

    if (s_shift_armed) {
        s_shift_passthru &= members;             // released -> chordable again on the next press
        const uint8_t active = members & ~s_shift_passthru;

        in.select = false;                       // the shift key itself is never Minus
        if (active & CH_START) { in.home = true;             in.start = false; }
        if (active & CH_UP)    { in.zl = in.zr = true;       in.up    = false; }
        if (active & CH_DOWN)  { in.capture = true;          in.down  = false; }
        if (active & CH_LEFT)  { in.zl = true;               in.left  = false; }
        if (active & CH_RIGHT) { in.zr = true;               in.right = false; }

        if (active && active != s_chord_active) ESP_LOGI(TAG, "chord -> %s", chord_name(active));
        s_chord_active = active;
        if (active) s_chord_fired = true;
    }

    // Pressing Select again inside the pulse window cuts the pulse short rather than extending Minus
    // under the new shift. Re-arming that fast costs a truncated Minus tap; not doing it would leak
    // Minus into whatever chord the second press forms, which is the thing the deferral exists to
    // prevent. Keeps the invariant simple: Select is never Minus while the shift is armed.
    if (!s_shift_armed && now_ms() < s_minus_pulse_until) in.select = true;

    s_prev_members = members;
}

// --- Gestures (hold-combos). Priority order resolves overlapping button sets. -------------------
enum Gesture { G_NONE, G_TRANSPORT, G_PROFILE, G_DIRMODE, G_CONFIG, G_FORGET, G_SLEEP };

static Gesture detect_gesture() {
    auto held = [](uint8_t b) { return nes->getButtonState(s_player, b); };
    bool a = held(NESController::BUTTON_A), b = held(NESController::BUTTON_B);
    bool sel = held(NESController::BUTTON_SELECT), sta = held(NESController::BUTTON_START);
    bool up = held(NESController::BUTTON_UP), dn = held(NESController::BUTTON_DOWN);

    if (sel && sta)        return G_TRANSPORT;   // Select+Start = switch transport (reboot)
    if (a && b && up)      return G_PROFILE;     // A+B+Up      = cycle profile
    if (a && b && dn)      return G_DIRMODE;     // A+B+Down    = cycle directional mode
    if (a && b && sel)     return G_CONFIG;      // A+B+Select  = BLE config/OTA mode (reboot)
    if (sel && !sta)       return G_FORGET;      // Select      = forget host / re-pair
    if (sta && !sel)       return G_SLEEP;       // Start       = sleep
    return G_NONE;
}

static void run_gesture(Gesture g) {
    switch (g) {
    case G_TRANSPORT: toggle_transport(); break;     // reboots
    case G_CONFIG:    enter_config_mode(); break;    // reboots into BLE config/OTA mode
    case G_PROFILE:   cycle_profile(); break;
    case G_DIRMODE:   cycle_directional_mode(); break;
    case G_FORGET:    ESP_LOGI(TAG, "forget host / re-pair"); bt::forget_host(); break;  // reboots
    case G_SLEEP:     enter_sleep(); break;           // does not return
    default: break;
    }
}

// Track the currently-held gesture and fire it once after kGestureHoldMs.
static void check_gestures() {
    static Gesture tracked = G_NONE;
    static int64_t since = 0;
    static bool fired = false;

    Gesture g = detect_gesture();

    // Block the Start-sleep gesture until Start has been released once after a Start-wake.
    if (g == G_SLEEP && s_start_release_required) g = G_NONE;
    if (!nes->getButtonState(s_player, NESController::BUTTON_START)) s_start_release_required = false;

    // Select+direction is a chord (see apply_chords), and it reads as G_FORGET here because that
    // gesture is Select-with-anything-but-Start. Holding a chord must not re-pair the stick, so a
    // chord disarms G_FORGET until Select is released. Select+Start reads as G_TRANSPORT and is
    // untouched: that hold still switches transport.
    if (g == G_FORGET && s_chord_fired) g = G_NONE;

    if (g != tracked) { tracked = g; since = now_ms(); fired = false; }
    if (g != G_NONE && !fired && now_ms() - since >= kGestureHoldMs) {
        fired = true;
        run_gesture(g);
    }
}

// --- LED indication ----------------------------------------------------------------------------
static void update_leds() {
    if (!s_led_auto) return;             // console owns the LEDs (led r|g|b ...; `led auto` restores)
    int64_t t = now_ms();
    bt::LinkState link = bt::link_state();

    // Blue = link: solid connected, blink advertising, off idle.
    if (link == bt::LINK_CONNECTED)        led_set(LED_BLUE, true);
    else if (link == bt::LINK_ADVERTISING) led_set(LED_BLUE, (t / 500) % 2 == 0);
    else                                   led_set(LED_BLUE, false);

    // Green = charging/full (only meaningful with battery hardware).
    if (battery::is_charging() && !battery::is_full()) led_set(LED_GREEN, (t / 500) % 2 == 0);
    else if (battery::is_full())                       led_set(LED_GREEN, true);
    else                                               led_set(LED_GREEN, false);

    // Red = low battery (only with battery hardware present).
    if (battery::present() && battery::level_percent() < 20) led_set(LED_RED, (t / 500) % 2 == 0);
    else                                                     led_set(LED_RED, false);
}

// --- Battery / sleep timers --------------------------------------------------------------------
static void check_timers() {
    int64_t t = now_ms();
    bool ext = battery::external_power();

    if (t - s_last_battery_ms > kBatteryPollMs) {
        s_last_battery_ms = t;
        if (battery::present()) bt::set_battery_level(battery::level_percent());
    }

    if (bt::connected() || ext) s_last_connected_ms = t;
    if (ext) nes->resetLastActivity();      // external power holds the device awake

    if (s_sleep_inhibit) {                  // bench: keep the timers from ever tripping sleep
        s_last_connected_ms = t;
        nes->resetLastActivity();
        return;
    }

    // Sleep on prolonged disconnection, or on prolonged inactivity while connected.
    if (!ext && !bt::connected() && t - s_last_connected_ms > kConnIdleTimeoutMs) {
        ESP_LOGI(TAG, "unconnected for %llds -> sleep", (long long)(kConnIdleTimeoutMs / 1000));
        enter_sleep();
    }
    if (!ext && t - nes->getLastActivityMs() > kIdleTimeoutMs) {
        ESP_LOGI(TAG, "no button activity for %llds -> sleep", (long long)(kIdleTimeoutMs / 1000));
        enter_sleep();
    }
}

// --- Build the active player's neutral input snapshot ------------------------------------------
static bt::NesInput read_input(uint8_t p) {
    bt::NesInput in;
    in.player = p;                     // BLE routes to this player's HID report; Classic ignores it
    in.a      = nes->getButtonState(p, NESController::BUTTON_A);
    in.b      = nes->getButtonState(p, NESController::BUTTON_B);
    in.select = nes->getButtonState(p, NESController::BUTTON_SELECT);
    in.start  = nes->getButtonState(p, NESController::BUTTON_START);
    in.up     = nes->getButtonState(p, NESController::BUTTON_UP);
    in.down   = nes->getButtonState(p, NESController::BUTTON_DOWN);
    in.left   = nes->getButtonState(p, NESController::BUTTON_LEFT);
    in.right  = nes->getButtonState(p, NESController::BUTTON_RIGHT);
    return in;
}

// Field-wise, because NesInput has padding a memcmp would read.
static bool same_input(const bt::NesInput& x, const bt::NesInput& y) {
    return x.a == y.a && x.b == y.b && x.select == y.select && x.start == y.start &&
           x.up == y.up && x.down == y.down && x.left == y.left && x.right == y.right &&
           x.home == y.home && x.capture == y.capture && x.zl == y.zl && x.zr == y.zr &&
           x.player == y.player;
}

// --- Console / bench hooks (app_control.hpp) ---------------------------------------------------
namespace app {
void request_sleep()        { s_sleep_requested = true; }
void enter_config_mode()    { ::enter_config_mode(); }   // arm the RTC flag + reboot into config mode
void set_sleep_inhibit(bool on) { s_sleep_inhibit = on; }
void set_led_auto(bool on)  { s_led_auto = on; if (on) update_leds(); }
uint8_t player()          { return s_player; }
uint8_t profile()         { return s_profile; }
uint8_t directional_mode(){ return s_dirmode; }
void set_profile(uint8_t p) {
    s_profile = p % bt::num_profiles();
    settings::set_profile(s_profile);
    ESP_LOGI(TAG, "profile -> %u (%s)", s_profile + 1, bt::profile_name(s_profile));
}
void set_directional_mode(uint8_t m) {
    s_dirmode = m % bt::num_directional_modes();
    settings::set_directional_mode(s_dirmode);
    ESP_LOGI(TAG, "directional mode -> %u (%s)", s_dirmode + 1, bt::directional_mode_name(s_dirmode));
}
ControllerDiag controller_diag() {
    return { s_raw_p1, s_raw_p2, !(s_raw_p1 == 0xFF && s_raw_p2 == 0xFF) };
}
} // namespace app

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (!handle_wake()) return;          // went back to sleep (does not return in practice)

    ESP_LOGI(TAG, "Bluetooth NES Advantage, gameplay firmware");
    settings::init();

    // BLE config/OTA mode: the A+B+Select gesture armed s_config_magic and rebooted. Honor it only
    // on a software reset (so power-on garbage in the uninitialized RTC var can't trigger it), and
    // clear it immediately so it's one-shot, the next plain reboot returns to gameplay. config::run()
    // brings up BLE only and never returns (it reboots on apply/forget/OTA-done or an idle timeout).
    bool enter_config = (esp_reset_reason() == ESP_RST_SW && s_config_magic == kConfigMagic);
    ESP_LOGI(TAG, "boot: reset_reason=%d, config %s", esp_reset_reason(),
             enter_config ? "ARMED -> entering config/OTA mode" : "not requested");
    s_config_magic = 0;
    if (enter_config) {
        led_init();
        config::run();
    }

    settings::apply_bt_identity();       // rotate the BT MAC per identity generation (before BT init)
    battery::init();
    led_init();

    nes = new NESController(NES_CLK_P1, NES_CLK_P2, NES_LATCH, NES_DATA_P1, NES_DATA_P2);
    nes->begin();

    // Wiring check first: a disconnected controller reads as all-buttons-held, which oscillates
    // player-select and trips the Select+Start transport gesture into a reboot loop. If nothing is
    // driving either DATA line, boot into BLE config/OTA mode instead, where the web page's live
    // tester shows the fault and can reflash. enter_config_mode() reboots (does not return).
    if (nes->diagnose() == NESController::NES_NO_SIGNAL) {
        ESP_LOGW(TAG, "no NES Advantage detected on J2 (both data lines idle-high) -> config mode");
        enter_config_mode();
    }

    nes->read();
    s_player = nes->getPlayerSelection();
    nes->setPlayerSelectionCallback(on_player_change);

    bt::Transport transport = (bt::Transport)settings::transport();
    ESP_LOGI(TAG, "active player P%d, transport %s, identity gen %u",
             s_player + 1, bt::transport_name(transport), settings::identity_generation());
    bt::init(transport);

    // Load + clamp persisted profile / directional mode to the active transport's tables.
    s_profile = settings::profile();
    if (s_profile >= bt::num_profiles()) s_profile = 0;
    s_dirmode = settings::directional_mode();
    if (s_dirmode >= bt::num_directional_modes()) s_dirmode = 0;
    ESP_LOGI(TAG, "profile %u (%s), directional mode %u (%s)",
             s_profile + 1, bt::profile_name(s_profile),
             s_dirmode + 1, bt::directional_mode_name(s_dirmode));

    s_last_connected_ms = now_ms();
    s_last_battery_ms   = now_ms();

    console_start();                     // UART REPL for bench + automated testing (serial_proxy send)

    char prev_raw[24] = "";
    TickType_t last_log = 0;
    while (true) {
        nes->read();

        // Publish the raw sample for the `diag` console command (bit7=A..bit0=R, 1=pressed;
        // 0xFF = deselected/disconnected sentinel).
        { uint8_t p1 = 0, p2 = 0;
          for (int i = 0; i < 8; i++) {
              p1 = (p1 << 1) | (nes->getButtonState(0, i) ? 1 : 0);
              p2 = (p2 << 1) | (nes->getButtonState(1, i) ? 1 : 0);
          }
          s_raw_p1 = p1; s_raw_p2 = p2; }

        // Resolve the Select-shift layer every poll, and gate the send on the RESOLVED snapshot
        // rather than on stateChanged: the chord layer breaks the 1:1 between the two. A Minus
        // pulse ends on a timer with no button moving (a raw-gated send would leave Minus stuck
        // down until the next press), and a chord changes what a held button means without the raw
        // state moving at all.
        bt::NesInput in = read_input(s_player);
        apply_chords(in);
        if (!s_have_sent || !same_input(in, s_last_sent)) {
            bt::set_input(in, s_profile, s_dirmode);
            s_last_sent = in;
            s_have_sent = true;
        }

        if (nes->stateChanged(0) || nes->stateChanged(1)) {
            // Unified controller log: both raw lines (bit order A B Sel Sta U D L R) plus the active
            // player. Rate-limited because a flickering controller otherwise spams UART and can
            // starve the BT task. A deselected line reads all '1' (the player-select sentinel).
            char r0[9], r1[9];
            for (int i = 0; i < 8; i++) { r0[i] = nes->getButtonState(0, i) ? '1' : '0';
                                          r1[i] = nes->getButtonState(1, i) ? '1' : '0'; }
            r0[8] = r1[8] = '\0';
            char line[24];
            snprintf(line, sizeof(line), "%s %s P%d", r0, r1, nes->getPlayerSelection() + 1);
            if (strcmp(line, prev_raw) != 0 && (xTaskGetTickCount() - last_log) >= pdMS_TO_TICKS(50)) {
                ESP_LOGI(TAG, "RAW P1=%s P2=%s sel=P%d", r0, r1, nes->getPlayerSelection() + 1);
                strcpy(prev_raw, line);
                last_log = xTaskGetTickCount();
            }
        }

        if (s_sleep_requested) { s_sleep_requested = false; enter_sleep(); }  // console `sleep`

        check_gestures();
        update_leds();
        check_timers();
        // Re-poll the 4021 every ~2 ms (needs CONFIG_FREERTOS_HZ=1000; see sdkconfig.defaults). The
        // read is ~0.4 ms at the default 80 MHz, so the loop is ~80% idle and leaves the BT task
        // plenty of CPU, and keeps press-to-sample latency near ~1 ms on average. The change-gated
        // log above (50 ms rate-limit) still protects the UART from a flickering controller.
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}