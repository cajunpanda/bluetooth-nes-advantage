// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>

// Hooks the bench console uses to drive the app_main state machine without physical gestures.
// Implemented in app_main.cpp; kept minimal so automated tests can poke state the same way a
// button hold would.

namespace app {

void    request_sleep();               // enter deep sleep from the main loop (like the Start gesture)
void    enter_config_mode();           // reboot into BLE config/OTA mode (like the A+B+Select gesture)
void    set_sleep_inhibit(bool on);    // true = suppress the idle/disconnect auto-sleep (bench)
void    set_profile(uint8_t p);        // apply + persist a button profile live (wraps to range)
void    set_directional_mode(uint8_t m); // apply + persist a directional mode live (wraps to range)
void    set_led_auto(bool on);         // false lets the console own the LEDs (update_leds() stands down)
uint8_t player();                      // active player, 0 = P1
uint8_t profile();                     // current button profile index
uint8_t directional_mode();            // current directional mode index

// Last raw controller sample, for the console `diag` command. p1/p2 are packed bit7=A..bit0=R
// (1 = pressed); 0xFF is the disconnected/deselected sentinel. connected is false when both lines
// read 0xFF (no controller on J2). Reads published state, so it never races the poll loop.
struct ControllerDiag { uint8_t p1; uint8_t p2; bool connected; };
ControllerDiag controller_diag();

} // namespace app
