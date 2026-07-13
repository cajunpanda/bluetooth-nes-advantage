// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include "esp_err.h"

// Serial-console control surface for bench and automated testing. A UART REPL (esp_console) whose
// commands wrap the settings_ / bt_ / battery_ APIs; it holds no state of its own. Coexists with
// ESP_LOG on UART0. Drive it from a monitor or with `serial_proxy.py send "<cmd>"`. Start after
// bt::init() and battery::init().
esp_err_t console_start();
