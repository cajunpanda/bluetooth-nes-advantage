// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once

// BLE configuration and OTA mode. A dedicated boot mode, distinct from the gameplay transports
// (bt_pro / bt_ble), that brings up BLE only and exposes a custom GATT service for a browser (Web
// Bluetooth) to:
//   * read device info and current settings, and change transport / profile / directional mode,
//   * push a new firmware image over the air into the inactive OTA slot (esp_ota).
//
// Entered from app_main via a hold-gesture that sets an RTC flag and reboots; one-shot, so a plain
// reboot (or an applied setting) returns to normal gameplay. config::run() does not return; it
// reboots on apply/forget/OTA-done or after an idle timeout.

namespace config {

void run();   // bring up the BLE config/OTA GATT service; never returns (reboots out of the mode)

}  // namespace config