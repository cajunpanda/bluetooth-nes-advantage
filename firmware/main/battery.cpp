// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "battery.hpp"
#include "esp_log.h"

static const char* TAG = "battery";

// This build reports the battery as absent (full, not charging, no external power), so the
// integration state machine treats power correctly: not charging, no external power, so the idle
// and sleep timers run normally instead of being held awake by a phantom charger. Real monitoring
// configures ADC1 on BATTERY_ADC_GPIO and the CHG/STBY inputs and reads the divider with EMA
// smoothing; enabling it needs a board to bench-calibrate the divider voltage curve.

namespace battery {

void init() {
    ESP_LOGW(TAG, "battery monitoring disabled, reporting absent (full, not charging)");
}

bool    present()        { return false; }
uint8_t level_percent()  { return 100; }
bool    is_charging()    { return false; }
bool    is_full()        { return false; }
bool    external_power() { return false; }

} // namespace battery