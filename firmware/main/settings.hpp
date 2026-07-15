// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>

// Persistent settings (NVS), namespace "nes-advantage". Stores the user-visible keys (profile,
// directional mode), the active BT transport (Classic Pro vs BLE; one radio at a time), and an
// identity generation counter that drives BT-MAC rotation on forget.
//
// Profiles and directional modes are compile-time tables owned by each transport; this module only
// stores the selected index. Indices are clamped to the active transport's table on read.

namespace settings {

void init();   // open the NVS namespace (call once, after nvs_flash_init)

uint8_t transport();                 // 0 = Classic Pro, 1 = BLE (see bt::Transport)
void    set_transport(uint8_t t);

uint8_t profile();                   // button-mapping profile index
void    set_profile(uint8_t p);

uint8_t directional_mode();          // d-pad / stick / both index
void    set_directional_mode(uint8_t m);

// BT identity generation. 0 = factory MAC; each forget bumps it so the device comes back with a
// fresh BT address and a forgotten host can no longer silently auto-reconnect to the stale bond.
uint8_t identity_generation();
void    bump_identity_generation();

// Apply the current identity generation to the chip's base MAC (so the BT address rotates with it).
// MUST be called before the BT controller is initialised (bt::init). Generation 0 = factory MAC.
void apply_bt_identity();

// Derive a stable BLE static-random address for the BLE transport, distinct from the Classic public
// MAC. BR/EDR and BLE otherwise share the one chip BT address, so a dual-mode host that paired us
// over Classic first folds the BLE HID into the same device record and never probes the LE HID
// services (fix-list #5). Giving BLE its own address makes the host see two separate devices. The
// address is derived from the factory MAC + identity generation (rotates on forget, like the base
// MAC) with the two MSBs forced to 0b11 (BLE static-random requirement, and guarantees it differs
// from the Espressif-OUI public MAC). out[] is a 6-byte esp_bd_addr_t.
void ble_static_rand_addr(uint8_t out[6]);

} // namespace settings