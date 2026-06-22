// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once

// Power / sleep management.
//
// Deep sleep: a ULP-FSM program clocks the CD4021 (LATCH/CLK on RTC GPIOs), shifts in 8 bits on
// DATA, compares to the last state, and wakes the main core only on a button change (about 5 to
// 150 uA standby, 20 to 100 ms latency). Requires the controller powered during sleep.

#include <cstdint>

namespace power {

void ulp_load();     // config RTC GPIOs, build + load + run the ULP-FSM 4021 poll, set wake period
void deep_sleep();   // arm ULP wakeup and enter deep sleep (does not return)

// Release the RTC-GPIO holds the ULP put on the NES pins, so the normal gpio driver (NESController)
// can drive/read them again after a wake. Call once on a full wake before NESController::begin().
void release_after_wake();

// Button bytes read by the ULP last poll (active-low: bit set = released). Bit order MSB-first:
// A B Select Start Up Down Left Right. Valid after a ULP wake.
uint8_t last_p1();
uint8_t last_p2();

// Convenience: was a given button pressed (active-low) in the ULP's last read, on either player
// line? `bit` is the NES poll index (0=A .. 3=Start .. 7=Right). Used on wake to ask "is Start
// held?" without bringing the controller fully up.
bool button_pressed_in_last_read(uint8_t bit);

} // namespace power