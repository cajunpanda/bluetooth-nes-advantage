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

} // namespace settings