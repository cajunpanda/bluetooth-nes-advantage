// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Board revision detection. See board.hpp.

#include "board.hpp"
#include "board_config.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs.h"

static const char* TAG = "board";

namespace {

constexpr char kNamespace[] = "nes-advantage";
constexpr char kKeyRev[]    = "boardRev";

// Survives deep sleep, so the probe only runs on a cold boot and not on every ULP button wake.
constexpr uint32_t kRtcMagic = 0x42524556;   // 'BREV'
RTC_DATA_ATTR uint32_t s_rtc_magic;
RTC_DATA_ATTR uint8_t  s_rtc_rev;

board::Rev s_rev = board::REV_UNKNOWN;

// True if the pin is held up by an external resistor (R7), false if it is left unconnected.
//
// Only ever drives the pin LOW. Driving it high would put the ESP32's source drive against the
// TP4056's open-drain CHRG transistor; driving low at worst fights R7 at 0.33 mA. Deliberately
// avoids the internal pull-down as a probe: 10k against a 30k to 80k internal pull lands on the
// V_IH threshold at the low end of that spread, whereas the recovery time differs by orders of
// magnitude and needs no margin.
bool has_external_pullup(gpio_num_t p) {
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = 1ULL << p;
    gpio_config(&out);
    gpio_set_level(p, 0);
    esp_rom_delay_us(200);              // drag the node down

    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;          // released, no internal pull
    in.pin_bit_mask = 1ULL << p;
    gpio_config(&in);
    esp_rom_delay_us(100);              // R7 recovers in <1 us; a float holds low for ms
    return gpio_get_level(p) == 1;
}

// Leave both candidates as readable inputs with their internal pull-ups on: the unconnected one
// must not float, and the wired one has to be readable before battery::init() configures it,
// because handle_wake() asks is_charging() on the way out of deep sleep.
//
// Deliberately not gpio_reset_pin(), which selects GPIO_MODE_DISABLE. That turns the input buffer
// off, so gpio_get_level() returns a constant 0, which on an active-low pin reads as "charging".
void park_candidates() {
    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.pin_bit_mask = (1ULL << CHG_STAT_REV20) | (1ULL << CHG_STAT_REV21);
    gpio_config(&in);
}

board::Rev nvs_read() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return board::REV_UNKNOWN;
    uint8_t v = 0xFF;
    if (nvs_get_u8(h, kKeyRev, &v) != ESP_OK) v = 0xFF;
    nvs_close(h);
    return (v == board::REV_2_0 || v == board::REV_2_1) ? (board::Rev)v : board::REV_UNKNOWN;
}

void nvs_write(board::Rev r) {
    if (nvs_read() == r) return;                       // no write unless it actually changed
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, kKeyRev, (uint8_t)r) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

} // namespace

namespace board {

void init() {
    if (s_rtc_magic == kRtcMagic && (s_rtc_rev == REV_2_0 || s_rtc_rev == REV_2_1)) {
        s_rev = (Rev)s_rtc_rev;                        // carried through deep sleep
        park_candidates();                             // the wake reset the pins to a disabled state
        return;
    }

    bool up_21 = has_external_pullup(CHG_STAT_REV21);
    bool up_20 = has_external_pullup(CHG_STAT_REV20);

    if (up_21 && !up_20) {
        s_rev = REV_2_1;
    } else if (up_20 && !up_21) {
        s_rev = REV_2_0;
    } else {
        // Neither candidate shows its pull-up, which means the TP4056 is actively holding CHRG low:
        // the board booted with a charge already in progress. Exactly one pin is wired on any real
        // board, so this resolves itself as soon as charging stops. Until then use the last known
        // answer, and fall back to 2.0, which simply skips the charger wake and behaves as before.
        s_rev = nvs_read();
        ESP_LOGW(TAG, "revision probe inconclusive (charging at boot?), using %s",
                 s_rev == REV_UNKNOWN ? "default 2.0" : rev_name());
        if (s_rev == REV_UNKNOWN) s_rev = REV_2_0;
        park_candidates();
        return;                                        // don't cache a guess
    }

    nvs_write(s_rev);
    s_rtc_rev = (uint8_t)s_rev;
    s_rtc_magic = kRtcMagic;
    park_candidates();
    ESP_LOGI(TAG, "PCB %s detected, CHG_STAT on GPIO%d (charger wake %s)",
             rev_name(), (int)chg_stat_gpio(), chg_wake_capable() ? "available" : "unavailable");
}

Rev rev() { return s_rev; }

const char* rev_name() {
    switch (s_rev) {
        case REV_2_0: return "2.0";
        case REV_2_1: return "2.1";
        default:      return "unknown";
    }
}

gpio_num_t chg_stat_gpio() {
    return s_rev == REV_2_1 ? (gpio_num_t)CHG_STAT_REV21 : (gpio_num_t)CHG_STAT_REV20;
}

bool chg_wake_capable() { return s_rev == REV_2_1; }

} // namespace board
