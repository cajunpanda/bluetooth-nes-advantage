// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "settings.hpp"
#include <cstring>
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

static const char* TAG = "settings";

namespace {
constexpr char kNamespace[] = "nes-advantage";
constexpr char kKeyTransport[] = "transport";
constexpr char kKeyProfile[]   = "profile";
constexpr char kKeyDirMode[]   = "directionalMode";
constexpr char kKeyIdentGen[]  = "identGen";

nvs_handle_t s_nvs = 0;

uint8_t get_u8(const char* key, uint8_t def) {
    uint8_t v = def;
    if (s_nvs) nvs_get_u8(s_nvs, key, &v);   // leaves v=def if key is missing
    return v;
}

void set_u8(const char* key, uint8_t v) {
    if (!s_nvs) return;
    ESP_ERROR_CHECK(nvs_set_u8(s_nvs, key, v));
    ESP_ERROR_CHECK(nvs_commit(s_nvs));
}
}  // namespace

namespace settings {

void init() {
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s, settings will use defaults", esp_err_to_name(err));
        s_nvs = 0;
    }
}

uint8_t transport()              { return get_u8(kKeyTransport, 0); }
void    set_transport(uint8_t t) { set_u8(kKeyTransport, t); }

uint8_t profile()                { return get_u8(kKeyProfile, 0); }
void    set_profile(uint8_t p)   { set_u8(kKeyProfile, p); }

uint8_t directional_mode()              { return get_u8(kKeyDirMode, 0); }
void    set_directional_mode(uint8_t m) { set_u8(kKeyDirMode, m); }

uint8_t identity_generation()    { return get_u8(kKeyIdentGen, 0); }
void    bump_identity_generation() {
    uint8_t g = identity_generation() + 1;   // wraps at 256; any change is enough to re-identify
    set_u8(kKeyIdentGen, g);
    ESP_LOGI(TAG, "identity generation -> %u", g);
}

void apply_bt_identity() {
    uint8_t gen = identity_generation();
    if (gen == 0) {
        ESP_LOGI(TAG, "BT identity: factory MAC (gen 0)");
        return;                                  // leave the factory base MAC untouched
    }
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        ESP_LOGW(TAG, "could not read factory MAC; identity not rotated");
        return;
    }
    mac[5] ^= gen;                               // rotate the low byte; stays a unicast address
    if (esp_base_mac_addr_set(mac) == ESP_OK) {
        ESP_LOGI(TAG, "BT identity: rotated base MAC -> %02x:%02x:%02x:%02x:%02x:%02x (gen %u)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], gen);
    }
}

void ble_static_rand_addr(uint8_t out[6]) {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};   // deterministic fallback if efuse fails
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        ESP_LOGW(TAG, "could not read factory MAC; BLE random address uses fallback base");
    }
    mac[5] ^= identity_generation();   // rotate with identity, matching apply_bt_identity's base MAC
    mac[0] |= 0xC0;                     // two MSBs = 0b11: BLE static random + distinct from public MAC
    memcpy(out, mac, 6);
}

} // namespace settings