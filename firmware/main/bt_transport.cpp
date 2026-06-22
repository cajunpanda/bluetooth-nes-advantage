// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Transport dispatch: pick one Ops table at boot and forward the neutral bt:: API to it.
#include "bt_transport.hpp"
#include "esp_log.h"

static const char* TAG = "bt";

namespace {
const bt::Ops*  s_ops    = nullptr;
bt::Transport   s_active = bt::TRANSPORT_CLASSIC;
}

namespace bt {

void init(Transport t) {
    if (t >= TRANSPORT_COUNT) {
        ESP_LOGW(TAG, "invalid transport %u -> Classic", t);
        t = TRANSPORT_CLASSIC;
    }
    s_active = t;
    s_ops = (t == TRANSPORT_BLE) ? bt_ble_ops() : bt_pro_ops();
    ESP_LOGI(TAG, "transport = %s", s_ops->name);
    s_ops->init();
}

Transport   active()                  { return s_active; }
const char* transport_name(Transport t) {
    switch (t) {
    case TRANSPORT_CLASSIC: return "Classic (Switch Pro)";
    case TRANSPORT_BLE:     return "BLE (NES Advantage)";
    default:                return "?";
    }
}

LinkState link_state() { return s_ops ? s_ops->link_state() : LINK_IDLE; }
bool      connected()  { return link_state() == LINK_CONNECTED; }

void set_input(const NesInput& in, uint8_t profile, uint8_t dir_mode) {
    if (s_ops) s_ops->set_input(in, profile, dir_mode);
}
void clear_input() { if (s_ops) s_ops->clear_input(); }

uint8_t     num_profiles()           { return s_ops ? s_ops->num_profiles() : 1; }
const char* profile_name(uint8_t i)  { return s_ops ? s_ops->profile_name(i) : "?"; }
uint8_t     num_directional_modes()          { return s_ops ? s_ops->num_directional_modes() : 1; }
const char* directional_mode_name(uint8_t i) { return s_ops ? s_ops->directional_mode_name(i) : "?"; }

void forget_host()                   { if (s_ops) s_ops->forget_host(); }
void set_battery_level(uint8_t pct)  { if (s_ops) s_ops->set_battery_level(pct); }

} // namespace bt