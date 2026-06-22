// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include "driver/gpio.h"

// Pin map for the production PCB (see docs/HARDWARE.md). Constraint: NES LATCH/CLK/DATA must be
// RTC-capable so the ULP can poll the 4021 in deep sleep. LATCH/CLK need RTC OUTPUT-capable pins
// (25/26/27/32/33). DATA stays on 32/33 (they have internal pulls; GPIO34-39 are input-only with
// no pulls). Avoid strapping pins; never use GPIO12. Battery sense must be ADC1 (32-39); ADC2 is
// unusable while the radio is on.

// Reserved for programming (do not reuse): U0TXD=GPIO1, U0RXD=GPIO3, plus EN + IO0 (auto-reset).
// These route to the TC2030 Tag-Connect footprint (J4), pinned for the
// TC2030-FTDI-C232HD-DDHSP-0-DTR USB-UART cable (DTR drives IO0, RTS drives EN for esptool reset).
// Charging is via the barrel jack (J1, +5V) only, no onboard USB.

// NES Advantage serial interface (CD4021)
#define NES_LATCH       GPIO_NUM_25   // RTC, output
#define NES_CLK_P1      GPIO_NUM_26   // RTC, output
#define NES_CLK_P2      GPIO_NUM_27   // RTC, output
#define NES_DATA_P1     GPIO_NUM_32   // RTC, input + pull-up
#define NES_DATA_P2     GPIO_NUM_33   // RTC, input + pull-up

// Battery sense (resistor divider), ADC1
#define BATTERY_ADC_GPIO GPIO_NUM_34  // ADC1_CH6, input-only
#define BATT_EN          GPIO_NUM_13  // Q2 gate: drive HIGH to ground the divider while sampling,
                                      // LOW in deep sleep so the 1M/1M divider draws ~0

// Status LEDs (active-low)
#define LED_RED         GPIO_NUM_19
#define LED_GREEN       GPIO_NUM_21
#define LED_BLUE        GPIO_NUM_22

// Charger status pins (open-drain, active-low; INPUT_PULLUP)
#define CHG_STAT        GPIO_NUM_23   // charging
#define STBY_STAT       GPIO_NUM_18   // standby / full