// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>

// Battery monitoring.
//
// Reads VBAT through the R12/R13 = 1M/1M divider on ADC1_CH6 (GPIO34), ground-switched by Q2
// (BATT_EN, GPIO13) so the divider only draws current while sampling. The sense node is filtered by
// C8 (100 nF); with a ~500k source impedance that is a ~50 ms RC, so a settled read asserts BATT_EN,
// waits, oversamples, then releases. State of charge is a coarse 1S-LiPo curve with EMA smoothing.
// Charge state comes from the TP4056 CHG (GPIO23) and STBY (GPIO18) open-drain status pins.
//
// present() is false until the first settled read; the integration layer keys off present() /
// external_power() so the rest of the firmware behaves correctly with or without a cell installed.

namespace battery {

void init();

bool    present();          // a cell is installed (VBAT above the presence threshold)
uint8_t level_percent();    // 0..100 coarse SoC (100 when absent)
bool    is_charging();      // TP4056 CHG asserted
bool    is_full();          // TP4056 STBY / fully charged
bool    external_power();   // charging || full, holds the device awake

// Bench / calibration helpers (blocking settled reads, ~300 ms each).
int read_mv();              // VBAT in mV after the divider + cal trim, or -1 if sense unavailable
int read_raw();             // averaged raw ADC counts at the sense node, or -1
int read_sense_mv();        // calibrated mV at the sense node (pre-divider), or -1

} // namespace battery
