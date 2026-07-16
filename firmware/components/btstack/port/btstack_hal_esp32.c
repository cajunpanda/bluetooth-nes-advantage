// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// The HAL hooks BTstack expects from the platform, for the parts of upstream's
// port/esp32/btstack_port_esp32.c this component does not vendor (that file also brings up BLE,
// mesh, audio, and a stdin console we have no use for - our transport does its own ~30-line
// equivalent). See ../README.md.

#include <stdint.h>
#include "hal_time_ms.h"

uint32_t esp_log_timestamp(void);

// HAVE_EMBEDDED_TIME_MS: the run loop's millisecond clock. Same implementation as upstream's
// esp32 port - esp_log_timestamp() is a free-running ms counter since boot.
uint32_t hal_time_ms(void) {
    return esp_log_timestamp();
}
