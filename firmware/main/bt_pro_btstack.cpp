// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Classic transport: Nintendo Switch Pro Controller emulation over BT Classic (BTstack).
// Wraps the bring-up, report descriptor, and 0x21/0x30 subcommand handshake in the bt::Ops
// transport interface:
//   * profiles and directional modes (NES buttons to Pro buttons / d-pad / left stick),
//   * the connection state machine: reconnect vs fresh-pair on boot, and identity/MAC refresh on
//     forget (forget drops bonds, bumps the identity generation, and reboots, so a forgotten host
//     cannot silently auto-reconnect to a stale BT address and skip the handshake).
//
// See docs/switch_pro_protocol.md for the wire format. The Switch identifies us by VID/PID, name,
// and CoD and drives everything over the subcommand handshake; the report descriptor only has to
// parse.
//
// Why BTstack and not Bluedroid (which this replaces): the Switch 2 pages a Bluedroid device with
// the right CoD, completes the ACL, then drops it (HCI reason 0x05) before ever starting Secure
// Simple Pairing - not fixable from the application side (Secure Connections host support and
// suppressing pre-auth L2CAP chatter were both verified on-air and neither unblocked it). BTstack
// gives us the whole host stack, and its defaults already satisfy what a real Pro does: silent on
// the ACL data plane until authenticated, SC host support advertised, no slave-initiated sniff.
//
// Threading: BTstack is single-threaded. Everything below runs in the run-loop task except
// pro_set_input/pro_clear_input/pro_set_battery_level (called from the app's poll loop), which
// publish state and hand off with btstack_run_loop_execute_on_main_thread().

#include "bt_transport.hpp"
#include "settings.hpp"

#include <atomic>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "nvs_flash.h"

// Individual headers, not the btstack.h umbrella: that one pulls in every profile and codec,
// including sources this component deliberately does not vendor (see components/btstack/README.md).
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_freertos.h"
#include "btstack_tlv.h"
#include "btstack_tlv_esp32.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"
#include "hci_transport_esp32_vhci.h"
#include "l2cap.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/device_id_server.h"
#include "classic/hid_device.h"
#include "classic/sdp_server.h"

static const char* TAG = "bt_pro";

// Bring-up switch: dumps every HCI packet as text to the serial console (benchmux-visible).
// The Switch 2 handshake is only debuggable at this level - keep it off for release builds.
// #define BTNA_HCI_DUMP 1

namespace {

// --- Internal Switch Pro state snapshot (built from NesInput + profile + directional mode) ------
struct ProState {
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false, zl = false, zr = false;
    bool minus = false, plus = false, home = false, capture = false;
    bool l_stick = false, r_stick = false;
    bool up = false, down = false, left = false, right = false;
    int16_t lx = 0, ly = 0;        // left stick, -1..+1 scaled (0 = centered)
};

volatile bt::LinkState s_link = bt::LINK_IDLE;
ProState s_state;                  // latest input snapshot (written by the app poll task)
uint8_t  s_battery = 0x80;         // Pro battery nibble<<4 | charge bits; 0x80 = full, not charging
bd_addr_t s_local_addr = {0};      // our BD_ADDR, for the device-info subcommand reply
bd_addr_t s_peer = {0};            // a stored host to actively page on boot (real-Pro reconnect)
bool s_have_bond = false;          // true means this boot is a reconnect, not a fresh pair
uint16_t s_hid_cid = 0;            // BTstack HID connection id; nonzero means connected

btstack_packet_callback_registration_t s_hci_event_cb;

// --- Device-initiated HID connect ("kick") -------------------------------------------------------
// A real Pro Controller advertises HIDReconnectInitiate and opens the HID L2CAP channels itself.
// Hosts that mimic the console's reconnect role (8BitDo USB Adapter 2, BlueRetro) therefore pair at
// GAP level and then wait forever for us to connect; only the Switch's Change Grip/Order screen and
// the older 8BitDo Retro Receiver initiate from their side. So: stay passive for a grace window
// (host-initiated still wins), then open the channels ourselves, with bounded retries so
// a dead host can't pin the radio awake. See docs/switch_pro_protocol.md "Connection direction".
btstack_timer_source_t s_kick_timer;
uint8_t s_kick_attempts = 0;
constexpr uint8_t  kKickMaxAttempts = 4;
// Host grace: long enough that hosts which open both channels themselves (Switch console, 8BitDo
// Retro Receiver, both well under 1 s) always win and cancel the kick, but short enough to beat
// BlueRetro, which opens CTRL and waits only ~2 s for the controller's INTR before tearing down
// and retrying - a 3 s grace loses that race and the two sides then collide (orphan L2CAP channel,
// subcommand replies lost, link dropped after ~30-60 s, repeat).
constexpr uint32_t kKickGraceMs = 1500;

// --- Profiles: which Pro buttons the NES face buttons produce ----------------------------------
// Select/Start always map to Minus/Plus. Directions are handled by the directional mode, not here.
struct ProProfile {
    const char* name;
    // Pro button each NES button activates, addressed by a small enum so the table stays readable.
    enum Btn { A, B, X, Y } nes_a, nes_b;
};
const ProProfile kProfiles[] = {
    // "Literal": A->A, B->B. Unambiguous in the Switch "Test Input Devices" screen.
    { "Literal", ProProfile::A, ProProfile::B },
    // "NSO NES": A->B, B->Y, the standard NSO NES layout (NES A = jump = Switch B).
    { "NSO NES", ProProfile::B, ProProfile::Y },
};
constexpr uint8_t kNumProfiles = sizeof(kProfiles) / sizeof(kProfiles[0]);

// --- Directional modes: where the NES d-pad goes ----------------------------------------------
enum ProDirMode : uint8_t { DIR_DPAD = 0, DIR_STICK = 1, DIR_BOTH = 2 };
const char* kDirModeNames[] = { "D-Pad", "Left Stick", "Both" };
constexpr uint8_t kNumDirModes = sizeof(kDirModeNames) / sizeof(kDirModeNames[0]);

void apply_profile_button(ProState& s, ProProfile::Btn which, bool pressed) {
    if (!pressed) return;
    switch (which) {
    case ProProfile::A: s.a = true; break;
    case ProProfile::B: s.b = true; break;
    case ProProfile::X: s.x = true; break;
    case ProProfile::Y: s.y = true; break;
    }
}

ProState map_input(const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    if (profile >= kNumProfiles) profile = 0;
    if (dir_mode >= kNumDirModes) dir_mode = DIR_DPAD;
    const ProProfile& p = kProfiles[profile];

    ProState s;
    apply_profile_button(s, p.nes_a, in.a);
    apply_profile_button(s, p.nes_b, in.b);
    s.minus = in.select;
    s.plus  = in.start;

    if (dir_mode == DIR_DPAD || dir_mode == DIR_BOTH) {
        s.up = in.up; s.down = in.down; s.left = in.left; s.right = in.right;
    }
    if (dir_mode == DIR_STICK || dir_mode == DIR_BOTH) {
        s.lx = (in.right ? 1 : 0) - (in.left ? 1 : 0);
        s.ly = (in.up ? 1 : 0) - (in.down ? 1 : 0);   // Pro stick: up is positive
    }
    return s;
}

// --- HID report descriptor (parses cleanly, declares 0x30/0x21/0x01/0x10) ----------------------
const uint8_t kProReportMap[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x06, 0x00, 0xFF, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x30,
    0x85, 0x30, 0x09, 0x30, 0x81, 0x02,
    0x85, 0x21, 0x09, 0x21, 0x81, 0x02,
    0x85, 0x3F, 0x09, 0x3F, 0x95, 0x0B, 0x81, 0x02,
    0x85, 0x01, 0x09, 0x01, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x91, 0x02,
    0xC0
};

constexpr uint16_t kVendorId  = 0x057E;          // Nintendo
constexpr uint16_t kProductId = 0x2009;          // Pro Controller
constexpr uint16_t kVersion   = 0x0001;
constexpr const char* kDeviceName = "Pro Controller";

// Full real-Pro Class of Device 0x002508, including the "Limited Discoverable" service-class bit.
// The Switch 2's Grip-menu inquiry pages a controller only if its CoD matches a real Pro's
// byte-for-byte (bench A/B: a BlueZ host presenting 0x002508 with a generic MAC and a junk-filled
// EIR was paged within 2 s of the console entering the screen; this device with 0x000508 and a
// byte-clean EIR was never paged. The Switch 1 accepts either, and neither console queries SDP
// before pairing, so the CoD is the whole gate). The ESP32 controller latches the CoD into its
// inquiry response when scan is enabled, so this must reach the controller before then: BTstack
// writes it during its HCI init sequence, i.e. any time before hci_power_control().
// Verify at the radio: `hcitool inq` -> 0x002508.
constexpr uint32_t kProCoD = 0x2508;

// --- Switch Pro report packing + handshake (see docs/switch_pro_protocol.md) -------------------
uint8_t s_timer = 0;
// A real Pro Controller powers up in simple input mode (report 0x3F, sent on change) and only
// streams full 0x30 reports after the host selects that mode with subcommand 0x03. The Switch
// console and the 8BitDo adapters send 0x03 during their handshake; BlueRetro never does and
// reads 0x3F reports, dropping a controller that stays silent (~20 s watchdog).
uint8_t s_input_mode = 0x3F;

const uint8_t kCal603D[] = {
    0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0xF0,0x07,0x7F,
    0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0x0F,0x0F, 0x00,0x00,0x00,0x00,0x00};
const uint8_t kCal6080[] = {
    0x5e,0x01,0x00,0x00,0xf1,0x0f, 0x19,0xd0,0x4c,0xae,0x40,0xe1,
    0x00,0x00,0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff,0xff,0xff};
const uint8_t kCal6098[] = {
    0x19,0xd0,0x4c,0xae,0x40,0xe1, 0x00,0x00,0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff,0xff,0xff};
const uint8_t kElapsed04[] = {0x00,0x6a,0x01,0xbb,0x01,0x93,0x01,0x95,0x01};

void pack_stick(uint8_t* o, uint16_t x, uint16_t y) {
    o[0] = x & 0xFF;
    o[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    o[2] = (y >> 4) & 0xFF;
}

void pack_buttons(uint8_t b[3], const ProState& s) {
    b[0] = (s.y?0x01:0)|(s.x?0x02:0)|(s.b?0x04:0)|(s.a?0x08:0)|(s.r?0x40:0)|(s.zr?0x80:0);
    b[1] = (s.minus?0x01:0)|(s.plus?0x02:0)|(s.r_stick?0x04:0)|(s.l_stick?0x08:0)|
           (s.home?0x10:0)|(s.capture?0x20:0);
    b[2] = (s.down?0x01:0)|(s.up?0x02:0)|(s.right?0x04:0)|(s.left?0x08:0)|(s.l?0x40:0)|(s.zl?0x80:0);
}

// Left stick: map -1/0/+1 to the 12-bit min/center/max the calibration expects.
void pack_left_stick(uint8_t* o, int16_t lx, int16_t ly) {
    auto axis = [](int16_t v) -> uint16_t {
        if (v > 0) return 3500;
        if (v < 0) return 600;
        return 2048;
    };
    pack_stick(o, axis(lx), axis(ly));
}

void fill_prefix(uint8_t* b, const ProState* st, const uint8_t btn[3]) {
    b[0] = s_timer++;
    b[1] = s_battery;
    b[2] = btn[0]; b[3] = btn[1]; b[4] = btn[2];
    if (st) pack_left_stick(b + 5, st->lx, st->ly);
    else    pack_stick(b + 5, 2048, 2048);
    pack_stick(b + 8, 2048, 2048);           // right stick centered (NES has none)
    b[11] = 0x80;
}

// --- Send path ---------------------------------------------------------------------------------
// BTstack sends when the interrupt channel is ready: request a can-send-now event, then build the
// frame in the callback. Subcommand replies take priority over the input stream.
//
// Replies are queued rather than held in a single slot: the VHCI transport hands the run loop
// every packet that arrived in one radio window before any can-send-now runs, so two output
// reports can be dispatched back to back, and a reply that could not ship inline (controller ACL
// buffers momentarily full) would be overwritten and lost - most likely during the console's
// back-to-back SPI reads, i.e. mid-handshake. Four slots is far more than the handshake needs.
constexpr uint8_t kReplyQueueLen = 4;
uint8_t s_reply[kReplyQueueLen][48];
uint8_t s_reply_head = 0;      // next reply to send
uint8_t s_reply_count = 0;     // queued replies
bool    s_can_send_requested = false;

void request_send() {
    if (s_hid_cid == 0 || s_can_send_requested) return;
    s_can_send_requested = true;
    hid_device_request_can_send_now_event(s_hid_cid);
}

// HID interrupt frames are self-framed: DATA|INPUT header byte, report id, then the payload.
void send_report(uint8_t report_id, const uint8_t* payload, uint16_t len) {
    uint8_t msg[2 + 48];
    if (len > sizeof(msg) - 2) len = sizeof(msg) - 2;
    msg[0] = 0xA1;                  // (HID_MESSAGE_TYPE_DATA << 4) | HID_REPORT_TYPE_INPUT
    msg[1] = report_id;
    memcpy(msg + 2, payload, len);
    hid_device_send_interrupt_message(s_hid_cid, msg, 2 + len);
}

void queue_reply(uint8_t ack, uint8_t subcmd, const uint8_t* payload, size_t plen) {
    if (s_reply_count >= kReplyQueueLen) {
        // Never seen in practice; loud because a dropped reply stalls the handshake and would
        // otherwise look like an unexplained pairing failure.
        ESP_LOGW(TAG, "reply queue full, dropping reply to subcmd 0x%02x", subcmd);
        return;
    }
    uint8_t* b = s_reply[(s_reply_head + s_reply_count) % kReplyQueueLen];
    memset(b, 0, 48);
    const uint8_t no_btn[3] = {0, 0, 0};
    fill_prefix(b, nullptr, no_btn);
    b[12] = ack;
    b[13] = subcmd;
    if (payload && plen) {
        if (plen > 48 - 14) plen = 48 - 14;
        memcpy(b + 14, payload, plen);
    }
    s_reply_count++;
    request_send();
}

void handle_spi_read(const uint8_t* args) {
    uint32_t addr = args[0] | (args[1] << 8) | (args[2] << 16) | ((uint32_t)args[3] << 24);
    uint8_t size = args[4];
    if (size > 0x1D) size = 0x1D;
    uint8_t payload[5 + 0x1D] = {0};
    memcpy(payload, args, 5);
    const uint8_t* blob = nullptr; size_t blen = 0;
    switch (addr) {
    case 0x603D: blob = kCal603D; blen = sizeof(kCal603D); break;
    case 0x6080: blob = kCal6080; blen = sizeof(kCal6080); break;
    case 0x6098: blob = kCal6098; blen = sizeof(kCal6098); break;
    case 0x8010: case 0x8028: memset(payload + 5, 0xFF, size); break;
    default: break;
    }
    if (blob) memcpy(payload + 5, blob, blen < size ? blen : size);
    queue_reply(0x90, 0x10, payload, 5 + size);
}

void handle_device_info() {
    uint8_t p[12] = {0x03, 0x48, 0x03, 0x02, 0,0,0,0,0,0, 0x01, 0x02};
    memcpy(p + 4, s_local_addr, 6);
    queue_reply(0x82, 0x02, p, sizeof(p));
}

// Output report 0x01 payload (report id already stripped): rumble counter, 8 rumble bytes, then
// the subcommand and its arguments.
void handle_subcommand(const uint8_t* data, int len) {
    if (len < 10) return;
    uint8_t subcmd = data[9];
    const uint8_t* args = data + 10;
    ESP_LOGI(TAG, "subcommand 0x%02x", subcmd);
    switch (subcmd) {
    case 0x02: handle_device_info(); break;
    case 0x10: if (len >= 15) handle_spi_read(args); break;
    case 0x04: queue_reply(0x83, 0x04, kElapsed04, sizeof(kElapsed04)); break;
    case 0x01: queue_reply(0x80, 0x01, nullptr, 0); break;
    case 0x03:
        // Set input report mode: 0x30 (or 0x31..) = full-report streaming, 0x3F = simple mode.
        if (len >= 11) s_input_mode = (args[0] == 0x3F) ? 0x3F : 0x30;
        ESP_LOGI(TAG, "input mode -> 0x%02x", s_input_mode);
        queue_reply(0x80, 0x03, nullptr, 0);
        break;
    case 0x08: queue_reply(0x80, 0x08, nullptr, 0); break;
    case 0x21: queue_reply(0x80, 0x21, nullptr, 0); break;
    case 0x30: queue_reply(0x80, 0x30, nullptr, 0); break;
    case 0x40: queue_reply(0x80, 0x40, nullptr, 0); break;
    case 0x48: queue_reply(0x80, 0x48, nullptr, 0); break;
    default:   queue_reply(0x80, subcmd, nullptr, 0); break;
    }
}

// Simple-mode (0x3F) input report: 2 button bytes, hat, then 4 x 16-bit axes (0x8000 = center).
void send_3f_report(const ProState& s) {
    uint8_t b[11] = {0};
    b[0] = (s.b?0x01:0)|(s.a?0x02:0)|(s.y?0x04:0)|(s.x?0x08:0)|(s.l?0x10:0)|(s.r?0x20:0)|
           (s.zl?0x40:0)|(s.zr?0x80:0);
    b[1] = (s.minus?0x01:0)|(s.plus?0x02:0)|(s.l_stick?0x04:0)|(s.r_stick?0x08:0)|
           (s.home?0x10:0)|(s.capture?0x20:0);
    static const uint8_t hat_lut[3][3] = {   // [down..up][left..right] -> hat code, 8 = neutral
        {5, 4, 3},    // down:  down-left, down, down-right
        {6, 8, 2},    // mid:   left, neutral, right
        {7, 0, 1},    // up:    up-left, up, up-right
    };
    int v = 1 + (s.up ? 1 : 0) - (s.down ? 1 : 0);
    int h = 1 + (s.right ? 1 : 0) - (s.left ? 1 : 0);
    b[2] = hat_lut[v][h];
    auto axis16 = [](int16_t a) -> uint16_t { return a > 0 ? 0xFFFF : a < 0 ? 0x0000 : 0x8000; };
    uint16_t lx = axis16(s.lx), ly16 = axis16((int16_t)-s.ly);   // 0x3F Y axis: down is positive
    b[3] = lx & 0xFF; b[4] = lx >> 8;
    b[5] = ly16 & 0xFF; b[6] = ly16 >> 8;
    b[7] = 0x00; b[8] = 0x80; b[9] = 0x00; b[10] = 0x80;         // right stick centered
    send_report(0x3F, b, sizeof(b));
}

// --- Report stream -----------------------------------------------------------------------------
// Replaces the old dedicated stream task: a run-loop timer keeps every BTstack call on the one
// thread. Ticks at 15 ms (>=66 Hz, what the Switch handshake expects); pro_set_input() nudges the
// run loop so a press ships immediately instead of waiting for the next tick.
btstack_timer_source_t s_stream_timer;
bool s_streaming = false;              // set once the link has settled
uint32_t s_settle_deadline_ms = 0;
uint32_t s_last_3f_ms = 0;
uint8_t s_prev_3f[3] = {0xFF, 0xFF, 0xFF};
constexpr uint32_t kStreamTickMs   = 15;
constexpr uint32_t kSettleMs       = 1500;   // let connection setup + sniff negotiation settle
constexpr uint32_t k3fKeepaliveMs  = 100;

void stream_timer_handler(btstack_timer_source_t* ts) {
    if (s_hid_cid != 0) {
        // btstack_time_delta, not >=: the ms clock is a uint32 that wraps, and a raw compare would
        // skip the settle entirely on the wrap.
        if (!s_streaming && btstack_time_delta(btstack_run_loop_get_time_ms(), s_settle_deadline_ms) >= 0) {
            s_streaming = true;
            ESP_LOGI(TAG, "link settled -> streaming (mode 0x%02x)", s_input_mode);
        }
        if (s_streaming) {
            if (s_input_mode == 0x30) {
                // Full mode: continuous ~66 Hz 0x30 stream (the Switch drops a quiet controller).
                request_send();
            } else {
                // Simple mode (real-Pro default; BlueRetro stays here): 0x3F on change, plus a
                // ~100 ms keepalive so a quiet controller is not reaped by host watchdogs.
                uint8_t btn[3];
                pack_buttons(btn, s_state);
                bool changed = memcmp(btn, s_prev_3f, sizeof(btn)) != 0;
                if (changed || (btstack_run_loop_get_time_ms() - s_last_3f_ms) >= k3fKeepaliveMs) {
                    request_send();
                }
            }
        }
        btstack_run_loop_set_timer(ts, kStreamTickMs);
        btstack_run_loop_add_timer(ts);
    }
}

void start_stream() {
    s_streaming = false;
    s_settle_deadline_ms = btstack_run_loop_get_time_ms() + kSettleMs;
    s_last_3f_ms = 0;
    memset(s_prev_3f, 0xFF, sizeof(s_prev_3f));
    // Start every connection with a clean send path. s_can_send_requested is a latch cleared only
    // by a can-send-now event, so a request left outstanding when the last link died (e.g. the
    // interrupt channel closed on its own, which does not raise CONNECTION_CLOSED) would block
    // every request_send() on this connection and stream nothing, silently.
    s_can_send_requested = false;
    s_reply_head = 0;
    s_reply_count = 0;
    btstack_run_loop_remove_timer(&s_stream_timer);
    btstack_run_loop_set_timer_handler(&s_stream_timer, stream_timer_handler);
    btstack_run_loop_set_timer(&s_stream_timer, kStreamTickMs);
    btstack_run_loop_add_timer(&s_stream_timer);
}

void handle_can_send_now() {
    s_can_send_requested = false;
    if (s_hid_cid == 0) return;

    if (s_reply_count > 0) {
        send_report(0x21, s_reply[s_reply_head], 48);
        s_reply_head = (s_reply_head + 1) % kReplyQueueLen;
        s_reply_count--;
        if (s_reply_count > 0) request_send();   // drain the rest, one per can-send-now
        return;
    }
    if (!s_streaming) return;

    ProState snapshot = s_state;           // copy so packing is consistent
    uint8_t btn[3];
    pack_buttons(btn, snapshot);

    if (s_input_mode == 0x30) {
        uint8_t b[48] = {0};
        fill_prefix(b, &snapshot, btn);
        send_report(0x30, b, sizeof(b));
    } else {
        send_3f_report(snapshot);
        memcpy(s_prev_3f, btn, sizeof(btn));
        s_last_3f_ms = btstack_run_loop_get_time_ms();
    }

    static uint8_t pb[3] = {0, 0, 0};
    static uint32_t last_tx = 0;
    if (memcmp(btn, pb, sizeof(btn)) != 0 &&
        (btstack_run_loop_get_time_ms() - last_tx) >= 50) {
        ESP_LOGI(TAG, "TX 0x%02x btn=%02x %02x %02x", s_input_mode, btn[0], btn[1], btn[2]);
        memcpy(pb, btn, sizeof(btn));
        last_tx = btstack_run_loop_get_time_ms();
    }
}

// --- Connection state machine -------------------------------------------------------------------
void kick_timer_handler(btstack_timer_source_t* ts) {
    if (s_hid_cid != 0) return;                       // host beat us to it
    static const bd_addr_t kZeroAddr = {0, 0, 0, 0, 0, 0};
    if (bd_addr_cmp(s_peer, kZeroAddr) == 0) return;  // no host to page yet
    if (s_kick_attempts >= kKickMaxAttempts) {
        ESP_LOGW(TAG, "device-initiated HID: giving up after %u attempts, back to passive",
                 s_kick_attempts);
        return;
    }
    s_kick_attempts++;
    ESP_LOGI(TAG, "host has not opened HID, device-initiating (attempt %u/%u)",
             s_kick_attempts, kKickMaxAttempts);
    uint16_t cid = 0;
    uint8_t status = hid_device_connect(s_peer, &cid);
    if (status != ERROR_CODE_SUCCESS) ESP_LOGW(TAG, "hid_device_connect: 0x%02x", status);
    // No blind retry timer here: the retry is driven by HID_SUBEVENT_CONNECTION_OPENED reporting
    // failure, which is the only signal that the page actually finished. Re-issuing on a timer
    // while the previous page is still running would stack connect attempts - hid_device_connect()
    // has no in-progress guard, so each one re-keys the singleton and leaves another L2CAP channel
    // in the list. When the page finally completes they ALL open, and every channel but the last
    // becomes an orphan the peer reaps ~30 s later, taking the session with it: exactly the
    // collision described above.
}

void arm_hid_kick(bool reset_attempts) {
    if (reset_attempts) s_kick_attempts = 0;
    btstack_run_loop_remove_timer(&s_kick_timer);
    btstack_run_loop_set_timer_handler(&s_kick_timer, kick_timer_handler);
    btstack_run_loop_set_timer(&s_kick_timer, kKickGraceMs);
    btstack_run_loop_add_timer(&s_kick_timer);
}

void cancel_hid_kick() {
    btstack_run_loop_remove_timer(&s_kick_timer);
    s_kick_attempts = 0;
}

void enter_discoverable() {
    gap_discoverable_control(1);
    gap_connectable_control(1);
}

// A stored link key means this boot is a reconnect (resume the same host); none means a fresh
// pair. Forget rotates our MAC, so after a forget there is no bond here and the (now
// differently-addressed) device pairs cleanly instead of half-reconnecting.
void load_bond() {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) {
        ESP_LOGW(TAG, "link key iterator unavailable");
        return;
    }
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    int n = 0;
    while (gap_link_key_iterator_get_next(&it, addr, key, &type)) {
        if (n == 0) {
            memcpy(s_peer, addr, sizeof(bd_addr_t));
            s_have_bond = true;
        }
        n++;
    }
    gap_link_key_iterator_done(&it);
    ESP_LOGI(TAG, "stored BT bonds at boot: %d", n);
}

// Output reports on the interrupt channel (how the console sends subcommands).
void on_report_data(uint16_t cid, hid_report_type_t type, uint16_t report_id,
                    int report_size, uint8_t* report) {
    (void)cid;
    if (type != HID_REPORT_TYPE_OUTPUT) return;
    if (report_id == 0x01) {
        handle_subcommand(report, report_size);
    } else if (report_id != 0x10) {      // 0x10 is rumble-only; we have no motor
        ESP_LOGW(TAG, "unexpected OUTPUT id=0x%02x len=%d", report_id, report_size);
    }
}

// The same subcommands, but sent as SET_REPORT on the control channel. Without this, BTstack's
// default handler silently discards them AND answers HANDSHAKE_SUCCESSFUL, so a host that uses
// this path would be told its subcommand landed and then wait forever for a 0x21 that never comes.
// Note the framing asymmetry against on_report_data: SET_REPORT does NOT strip the report id
// (hid_device.c passes &packet[1], id included), so skip it by hand.
void on_set_report(uint16_t cid, hid_report_type_t type, int report_size, uint8_t* report) {
    (void)cid;
    if (type != HID_REPORT_TYPE_OUTPUT || report_size < 1) return;
    uint8_t report_id = report[0];
    if (report_id == 0x01) {
        handle_subcommand(report + 1, report_size - 1);
    } else if (report_id != 0x10) {
        ESP_LOGW(TAG, "unexpected SET_REPORT id=0x%02x len=%d", report_id, report_size);
    }
}

void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        gap_local_bd_addr(s_local_addr);
        ESP_LOGI(TAG, "BTstack up at %s -> connectable + discoverable", bd_addr_to_str(s_local_addr));
        enter_discoverable();
        s_link = bt::LINK_ADVERTISING;
        load_bond();
        // Start passive for both fresh pair and reconnect: hosts that initiate HID themselves (the
        // Switch "Change Grip/Order" screen, the 8BitDo Retro Receiver) get the grace window and
        // connect normally. Hosts that mimic the console's reconnect role and wait for the
        // controller to open the channels (8BitDo USB Adapter 2, BlueRetro, a console after a power
        // cycle) get the bounded device-initiated kick. See docs/switch_pro_protocol.md
        // "Connection direction".
        if (s_have_bond) {
            ESP_LOGI(TAG, "reconnect: discoverable, host may initiate or we kick after grace");
            arm_hid_kick(true);
        } else {
            ESP_LOGI(TAG, "fresh pair: discoverable, waiting for a host");
        }
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        // Just-Works pairing: gap_ssp_set_auto_accept(1) answers this for us; log it because it is
        // the first sign the console is actually pairing rather than probing.
        ESP_LOGI(TAG, "SSP user confirmation (auto-accepted)");
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        // The authoritative peer for the kick, captured before any HID connection. A reconnect
        // resumes on a stored link key with no fresh pairing, so this is the only event that
        // reports the real host on both fresh and resumed links; with two bonds stored, relying on
        // the first bond instead would page the wrong host.
        if (hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            hci_event_connection_complete_get_bd_addr(packet, s_peer);
            ESP_LOGI(TAG, "ACL up: %s", bd_addr_to_str(s_peer));
        }
        break;

    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
        uint8_t st = hci_event_simple_pairing_complete_get_status(packet);
        ESP_LOGI(TAG, "SSP complete: status 0x%02x", st);
        if (st == ERROR_CODE_SUCCESS) {
            // A freshly paired host may now sit and wait for the controller to open HID (8BitDo
            // USB Adapter 2, BlueRetro). Nothing else arms the kick on a fresh pair: there is no
            // bond at boot to trigger it, and with no HID connection there is no disconnect to
            // trigger it either, so without this the device waits forever.
            hci_event_simple_pairing_complete_get_bd_addr(packet, s_peer);
            arm_hid_kick(true);
        }
        break;
    }

    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                ESP_LOGW(TAG, "HID connect failed, status 0x%02x", status);
                // Only tear down if we have no link. A kick that loses the race to the host's own
                // incoming connection reports its failure here *after* that connection opened;
                // acting on it would drop a working link and re-advertise on top of it, with no
                // disconnect event left to recover from.
                if (s_hid_cid == 0) {
                    s_link = bt::LINK_ADVERTISING;
                    enter_discoverable();   // let the host page us again
                    // This is the page's completion signal, so it is the only safe moment to retry
                    // (see kick_timer_handler). arm_hid_kick keeps the attempt count.
                    arm_hid_kick(false);
                }
                break;
            }
            s_hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, s_peer);
            s_input_mode = 0x3F;   // every fresh connection starts in simple mode, like a real Pro
            s_link = bt::LINK_CONNECTED;
            cancel_hid_kick();
            gap_discoverable_control(0);
            gap_connectable_control(0);
            ESP_LOGI(TAG, "HID connected to %s (%s)", bd_addr_to_str(s_peer),
                     hid_subevent_connection_opened_get_incoming(packet) ? "host-initiated"
                                                                         : "device-initiated");
            start_stream();
            break;
        }
        case HID_SUBEVENT_CONNECTION_CLOSED:
            ESP_LOGW(TAG, "HID disconnected -> re-advertise");
            s_hid_cid = 0;
            s_streaming = false;
            s_reply_count = 0;
            s_reply_head = 0;
            s_can_send_requested = false;
            btstack_run_loop_remove_timer(&s_stream_timer);
            s_link = bt::LINK_ADVERTISING;
            enter_discoverable();
            arm_hid_kick(true);   // a power-cycled host waits for us to re-join
            break;
        case HID_SUBEVENT_CAN_SEND_NOW:
            handle_can_send_now();
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

// --- Cross-thread input handoff ------------------------------------------------------------------
// pro_set_input runs on the app's poll task; BTstack must only be touched from the run loop.
// Publish the snapshot, then ask the run loop to ship it. The pending flag coalesces bursts: at
// most one callback is in flight, and it always sees the newest state.
std::atomic<bool> s_input_pending{false};
std::atomic<bool> s_run_loop_ready{false};   // set once the run loop is initialized and armed

void on_input_changed(void* context) {
    (void)context;
    s_input_pending.store(false);
    if (s_hid_cid != 0 && s_streaming) request_send();
}

btstack_context_callback_registration_t s_input_cb = {
    .item     = nullptr,
    .callback = &on_input_changed,
    .context  = nullptr,
};

void nudge_run_loop() {
    // The app polls inputs from its first tick, which can precede the stack coming up.
    if (!s_run_loop_ready.load()) return;
    if (s_input_pending.exchange(true)) return;   // one already queued; it will see the new state
    btstack_run_loop_execute_on_main_thread(&s_input_cb);
}

// --- Ops implementation -------------------------------------------------------------------------
// The whole stack is brought up here, on the run-loop task, and not in pro_init(): the FreeRTOS
// run loop records the task that calls btstack_run_loop_init() as the one to notify for any
// cross-thread work (btstack_run_loop_freertos.c: btstack_run_loop_task = xTaskGetCurrentTaskHandle()).
// Initializing from the app's task would point every wakeup at the wrong task - including the VHCI
// transport's own packet delivery, which hands each incoming HCI packet to the run loop that way -
// and the stack would sit silent forever, never reaching HCI_STATE_WORKING.
void btstack_task(void*) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_freertos_get_instance());
    hci_init(hci_transport_esp32_vhci_get_instance(), nullptr);

#ifdef BTNA_HCI_DUMP
    hci_dump_init(hci_dump_embedded_stdout_get_instance());
#endif

    // Link keys persist in NVS (namespace "BTstack"), so a paired console is remembered across
    // reboots and we know on the next boot whether to reconnect or wait for a fresh pair.
    const btstack_tlv_t* tlv = btstack_tlv_esp32_get_instance();
    btstack_tlv_set_instance(tlv, nullptr);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, nullptr));

    // GAP: everything here lands in BTstack's HCI init sequence, before scan is enabled.
    gap_set_class_of_device(kProCoD);
    gap_set_local_name(kDeviceName);
    // Accept a master's sniff and role switch, but never initiate either: a real Pro Controller is
    // a slave that lets the host drive power management (the 8BitDo NES receiver refuses a
    // device-initiated sniff outright, HCI status 0x24). BTstack only ever initiates sniff when
    // asked, so allowing the policy is safe.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    // Page timeout ~5.1 s instead of BTstack's ~15.4 s default: the kick pages a host that may
    // simply be gone (powered-off console), and a 15 s page pins the radio awake and stretches the
    // 4-attempt kick sequence past a minute.
    gap_set_page_timeout(0x2000);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);   // "just works", no PIN
    gap_ssp_set_auto_accept(1);
    // Secure Connections host support: the Switch 2 reads our LMP feature pages right after the
    // ACL comes up. Bluedroid could not advertise this bit without patching (ESP-IDF vetoes SC on
    // ESP32 as an AES-CCM precaution); BTstack advertises it whenever the controller supports it,
    // which this one does. The Switch 1 and 8BitDo hosts ignore the bit.
    gap_secure_connections_enable(true);

    l2cap_init();
    sdp_init();

    // HID SDP record. Subclass 0x2508 and HIDReconnectInitiate mirror a real Pro; the console
    // reads this only after the link is up, so it is not part of the discovery gate.
    static uint8_t hid_sdp_buf[500];
    memset(hid_sdp_buf, 0, sizeof(hid_sdp_buf));
    hid_sdp_record_t hid_params = {
        .hid_device_subclass      = kProCoD,
        .hid_country_code         = 33,       // US
        .hid_virtual_cable        = 1,
        .hid_remote_wake          = 1,
        .hid_reconnect_initiate   = 1,
        .hid_normally_connectable = false,
        .hid_boot_device          = false,
        .hid_ssr_host_max_latency = 0xFFFF,   // no sniff subrating request from us
        .hid_ssr_host_min_timeout = 0xFFFF,
        .hid_supervision_timeout  = 3200,
        .hid_descriptor           = kProReportMap,
        .hid_descriptor_size      = sizeof(kProReportMap),
        .device_name              = kDeviceName,
    };
    hid_create_sdp_record(hid_sdp_buf, sdp_create_service_record_handle(), &hid_params);
    sdp_register_service(hid_sdp_buf);

    // Device ID record: the Switch identifies a Pro Controller by this VID/PID.
    static uint8_t did_sdp_buf[100];
    memset(did_sdp_buf, 0, sizeof(did_sdp_buf));
    device_id_create_sdp_record(did_sdp_buf, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_USB, kVendorId, kProductId, kVersion);
    sdp_register_service(did_sdp_buf);

    hid_device_init(false, sizeof(kProReportMap), kProReportMap);
    // The console's 0x01 output reports are shorter than the 48 bytes our descriptor declares.
    // BTstack drops mismatched reports by default, which would silently swallow every subcommand.
    hid_device_accept_truncated_hid_reports(true);
    hid_device_register_packet_handler(&packet_handler);
    hid_device_register_report_data_callback(&on_report_data);
    hid_device_register_set_report_callback(&on_set_report);

    s_hci_event_cb.callback = &packet_handler;
    hci_add_event_handler(&s_hci_event_cb);

    ESP_LOGI(TAG, "init done, pair from the Switch 'Change Grip/Order' screen or an 8BitDo receiver");

    s_run_loop_ready.store(true);      // the app's input nudges are safe to queue from here on
    hci_power_control(HCI_POWER_ON);   // opens the transport: controller init + enable, Classic-only
    btstack_run_loop_execute();        // never returns
}

void pro_init() {
    ESP_LOGI(TAG, "Switch Pro / BT Classic, bringing up BTstack (BR/EDR only this boot)");
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // one radio at a time: free the BLE half
    // Everything else runs on the run-loop task; see the note above btstack_task.
    xTaskCreatePinnedToCore(btstack_task, "btstack", 6144, nullptr, 5, nullptr, 0);
}

bt::LinkState pro_link_state() { return s_link; }

void pro_set_input(const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    s_state = map_input(in, profile, dir_mode);
    // app_main only calls set_input() on an actual button-state change, so ship this snapshot now
    // instead of waiting for the stream timer's next 15 ms tick.
    nudge_run_loop();
}

void pro_clear_input() {
    s_state = ProState{};
    nudge_run_loop();
}

uint8_t     pro_num_profiles()          { return kNumProfiles; }
const char* pro_profile_name(uint8_t i) { return i < kNumProfiles ? kProfiles[i].name : "?"; }
uint8_t     pro_num_dir_modes()         { return kNumDirModes; }
const char* pro_dir_mode_name(uint8_t i){ return i < kNumDirModes ? kDirModeNames[i] : "?"; }

void pro_set_battery_level(uint8_t pct) {
    // Pro input reports carry a 4-bit battery nibble in the high bits of byte 1 (8=full .. 0=empty).
    uint8_t nib = pct >= 70 ? 0x8 : pct >= 40 ? 0x6 : pct >= 20 ? 0x4 : pct >= 10 ? 0x2 : 0x0;
    s_battery = nib << 4;
}

void pro_forget_host() {
    // Forget: drop every bond and rotate our BT identity, then reboot. The reboot brings us up with
    // a new MAC and no bond, so the previously-paired host no longer recognises us; it cannot
    // auto-reconnect to a stale address and skip the handshake.
    ESP_LOGW(TAG, "forget host -> remove bonds + rotate identity + reboot");
    bt::clear_all_bonds();   // BTstack link keys and the BLE table: the identity bump invalidates all
    settings::bump_identity_generation();
    vTaskDelay(pdMS_TO_TICKS(200));   // let NVS commit
    esp_restart();
}

const bt::Ops s_ops = {
    .name                  = "Classic (Switch Pro)",
    .init                  = pro_init,
    .link_state            = pro_link_state,
    .set_input             = pro_set_input,
    .clear_input           = pro_clear_input,
    .num_profiles          = pro_num_profiles,
    .profile_name          = pro_profile_name,
    .num_directional_modes = pro_num_dir_modes,
    .directional_mode_name = pro_dir_mode_name,
    .forget_host           = pro_forget_host,
    .set_battery_level     = pro_set_battery_level,
};

} // namespace

const bt::Ops* bt_pro_ops() { return &s_ops; }
