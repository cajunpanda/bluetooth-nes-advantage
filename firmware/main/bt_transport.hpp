// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>

// Transport-neutral Bluetooth HID interface. The app (app_main) talks only to `bt::` and never to a
// concrete radio. Exactly one transport is brought up per boot, Classic Switch Pro (bt_pro) or BLE
// gamepad (bt_ble), chosen from NVS (settings::transport). Switching transports is a write-NVS plus
// reboot, so only one radio and identity is ever live.
//
// Each transport owns its own button-mapping profiles and directional modes (the wire formats
// differ: Pro buttons plus analog sticks vs a 12-button HID report with a hat plus axes), so the
// profile and dir-mode naming and counts come from the active transport; the app only holds the
// indices.

namespace bt {

enum Transport : uint8_t {
    TRANSPORT_CLASSIC = 0,   // Switch Pro Controller over BT Classic (bt_pro)
    TRANSPORT_BLE     = 1,   // "NES Advantage" BLE GATT HID gamepad (bt_ble)
    TRANSPORT_COUNT   = 2,
};

enum LinkState : uint8_t { LINK_IDLE = 0, LINK_ADVERTISING = 1, LINK_CONNECTED = 2 };

// One active-player snapshot in NES terms; the transport applies the profile + directional mode
// when packing its wire report. (Player select is resolved by the caller before this.)
//
// `player` is the 0-based selected player (P1=0, P2=1). Classic ignores it (one Pro Controller).
// BLE uses it to route the snapshot to that player's HID report so PCs/emulators see P1 and P2 as
// two separate gamepads, the original Advantage's take-turns hand-off.
//
// home/capture/zl/zr are virtual: no NES button produces them. The app's Select-shift chord layer
// synthesises them and clears the members it consumed, so a transport can treat them as if they
// were real inputs. They are not profile-mapped (a profile picks which Pro/HID button a NES button
// means; these have only one meaning), and they are only ever set for the active player.
struct NesInput {
    bool a = false, b = false, select = false, start = false;
    bool up = false, down = false, left = false, right = false;
    bool home = false, capture = false, zl = false, zr = false;
    uint8_t player = 0;
};

// A transport implementation. bt_pro / bt_ble each expose a pointer to one of these.
struct Ops {
    const char* name;
    void        (*init)();
    LinkState   (*link_state)();
    void        (*set_input)(const NesInput& in, uint8_t profile, uint8_t dir_mode);
    void        (*clear_input)();                  // zero the current report (player switch)
    uint8_t     (*num_profiles)();
    const char* (*profile_name)(uint8_t i);
    uint8_t     (*num_directional_modes)();
    const char* (*directional_mode_name)(uint8_t i);
    void        (*forget_host)();                  // forget bond + rotate identity + reboot
    void        (*set_battery_level)(uint8_t pct);
};

// Bring up the selected transport (must be called once). Falls back to Classic on a bad value.
void init(Transport t);

Transport   active();
const char* transport_name(Transport t);

// Thin forwarders to the active transport's Ops (safe no-ops before init()).
LinkState   link_state();
bool        connected();
void        set_input(const NesInput& in, uint8_t profile, uint8_t dir_mode);
void        clear_input();
uint8_t     num_profiles();
const char* profile_name(uint8_t i);
uint8_t     num_directional_modes();
const char* directional_mode_name(uint8_t i);
void        forget_host();
void        set_battery_level(uint8_t pct);

// Remove EVERY stored bond on BOTH transports (BR/EDR and BLE), not just the active one. A forget
// rotates our BT identity, which invalidates all bonds at once; clearing only the live transport
// leaves the other's table to strand a host that can no longer recognise our rotated address on the
// next transport switch. Both bond tables are host-side (loaded from the shared
// bt_config NVS at bluedroid enable), so this is reachable with either radio live. Call from a
// transport's forget_host, before the identity bump + reboot.
void        clear_all_bonds();

} // namespace bt

// Implemented by the two transports (kept out of the header to avoid pulling Bluedroid in here).
const bt::Ops* bt_pro_ops();
const bt::Ops* bt_ble_ops();