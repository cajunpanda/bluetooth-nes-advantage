// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Transport dispatch: pick one Ops table at boot and forward the neutral bt:: API to it.
#include "bt_transport.hpp"

#include <cstdlib>
#include "esp_log.h"
#include "esp_bt.h"
#include "nvs.h"
#if defined(CONFIG_BT_CLASSIC_ENABLED)
#include "esp_gap_bt_api.h"
#endif
#include "esp_gap_ble_api.h"

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

void clear_all_bonds() {
    // See the header: forget invalidates bonds on both radios at once, so clear both tables here
    // regardless of which one is live this boot. Bonds live in NVS, so removal is reachable even
    // though only one controller is up; a stack with no bonds simply clears nothing. Guarded so a
    // single-mode build still links.

    // BTstack (Classic Switch Pro) keeps its link keys in its own NVS namespace. Erase the whole
    // namespace rather than calling gap_delete_all_link_keys(): that API only works on the boot
    // where BTstack is the live stack, and it must run on the run loop; this works from any boot.
    nvs_handle_t h;
    if (nvs_open("BTstack", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) {
            ESP_LOGI(TAG, "forget: cleared BTstack link keys");
        }
        nvs_close(h);
    }
#if defined(CONFIG_BT_CLASSIC_ENABLED)
    int nc = esp_bt_gap_get_bond_device_num();
    if (nc > 0) {
        esp_bd_addr_t list[8];
        int n = nc > 8 ? 8 : nc;
        if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK && n > 0) {
            for (int i = 0; i < n; i++) esp_bt_gap_remove_bond_device(list[i]);
            ESP_LOGI(TAG, "forget: cleared %d BR/EDR bond(s)", n);
        }
    }
#endif
#if defined(CONFIG_BT_BLE_ENABLED)
    int nb = esp_ble_get_bond_device_num();
    if (nb > 0) {
        esp_ble_bond_dev_t* bl = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * nb);
        if (bl && esp_ble_get_bond_device_list(&nb, bl) == ESP_OK) {
            for (int i = 0; i < nb; i++) esp_ble_remove_bond_device(bl[i].bd_addr);
            ESP_LOGI(TAG, "forget: cleared %d BLE bond(s)", nb);
        }
        free(bl);
    }
#endif
}

} // namespace bt