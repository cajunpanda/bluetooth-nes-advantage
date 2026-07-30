// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>
#include "driver/gpio.h"

// Board revision detection.
//
// PCB 2.1 moved the TP4056 CHRG status line from GPIO23 to GPIO4 so the charger can wake the SoC
// out of deep sleep (ext1 needs an RTC-capable pin; GPIO23 is not one). Everything else is
// identical, and there is no board-ID pin, so the revision is probed at runtime and one build runs
// on both.
//
// The probe leans on the fact that R7 pulls the wired CHG_STAT up to +3.3V while the other
// revision's candidate pin is left unconnected: drive a candidate low, release it, and a 10k
// pull-up restores it in well under a microsecond while a floating pin stays low for milliseconds.
// See init() for the one ambiguous case.

namespace board {

enum Rev : uint8_t {
    REV_2_0 = 0,        // CHG_STAT on GPIO23, no charger wake
    REV_2_1 = 1,        // CHG_STAT on GPIO4, charger can wake from deep sleep
    REV_UNKNOWN = 0xFF,
};

// Probe (or reuse a cached result). Call once, early, before anything configures CHG_STAT.
// Needs nvs_flash_init() to have run; safe to call before settings::init().
void init();

Rev         rev();
const char* rev_name();
gpio_num_t  chg_stat_gpio();     // the wired TP4056 CHRG pin for this board
bool        chg_wake_capable();  // CHG_STAT is RTC-capable, so ext1 can wake on charger insert

} // namespace board
