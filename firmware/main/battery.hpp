// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>

// Battery monitoring.
//
// This build reports the battery as absent: full, not charging, not externally powered, and
// present() returns false. The integration layer keys off present()/external_power() so the rest of
// the firmware behaves correctly without battery hardware (it will not pretend to charge and it will
// not suppress sleep). Real monitoring reads a resistor divider on ADC1 (coarse state of charge with
// EMA smoothing) plus the TP4056 CHG/STBY status pins.

namespace battery {

void init();

bool    present();          // false when the battery subsystem is not populated
uint8_t level_percent();    // 0..100 (100 when absent)
bool    is_charging();      // CHG asserted (always false when absent)
bool    is_full();          // STBY / fully charged (always false when absent)
bool    external_power();   // charging || full, holds the device awake (always false when absent)

} // namespace battery