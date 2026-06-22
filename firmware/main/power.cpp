// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Power / sleep management. See power.hpp.
//
// The ULP-FSM bit-bangs the CD4021 protocol directly on RTC GPIOs (pulse LATCH, then 8x read DATA
// and clock CLK), assembles the 8 button bits for both players, stores them in RTC slow memory, and
// wakes the SoC only when the state changed since the last poll. The main CPU and radio stay off;
// only the ULP and RTC IO are alive.
//
// The macro program is built as a runtime C array (esp32/ulp.h via ulp.h), with no ULP-assembly step.

#include "power.hpp"

#include <cstring>
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/soc.h"            // RTC_SLOW_MEM
#include "soc/rtc_cntl_reg.h"   // RTC_CNTL_* (timer macros)
#include "soc/rtc_io_reg.h"     // RTC_GPIO_OUT_REG / RTC_GPIO_IN_REG + data shift bases
#include "soc/rtc_io_channel.h" // RTCIO_GPIOxx_CHANNEL
#include "ulp.h"                // FSM macros + ulp_process_macros_and_load / ulp_run / period
#include "board_config.h"       // NES_LATCH / NES_CLK_* / NES_DATA_*

namespace power {

// --- RTC-GPIO register bit positions (data field starts at bit 14 == channel 0) ---
static constexpr int kLatchBit = RTC_GPIO_OUT_DATA_S + RTCIO_GPIO25_CHANNEL;  // GPIO25, out  -> 20
static constexpr int kClk1Bit  = RTC_GPIO_OUT_DATA_S + RTCIO_GPIO26_CHANNEL;  // GPIO26, out  -> 21
static constexpr int kClk2Bit  = RTC_GPIO_OUT_DATA_S + RTCIO_GPIO27_CHANNEL;  // GPIO27, out  -> 31
static constexpr int kData1Bit = RTC_GPIO_IN_NEXT_S  + RTCIO_GPIO32_CHANNEL;  // GPIO32, in   -> 23
static constexpr int kData2Bit = RTC_GPIO_IN_NEXT_S  + RTCIO_GPIO33_CHANNEL;  // GPIO33, in   -> 22

// RTC_SLOW_MEM words (inside the reserved ULP region, past the program) for ULP -> CPU results.
static constexpr int kAddrP1 = 200;
static constexpr int kAddrP2 = 201;

// ULP wakeup-timer period = poll interval = worst-case wake latency (targets 20 to 100 ms).
// Shorter means a snappier wake but higher standby current.
static constexpr uint32_t kUlpPeriodUs = 30'000;   // 30 ms

static void config_rtc_gpio() {
    const gpio_num_t outs[] = {NES_LATCH, NES_CLK_P1, NES_CLK_P2};
    for (gpio_num_t p : outs) {
        rtc_gpio_init(p);
        rtc_gpio_set_direction(p, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(p, 0);
    }
    const gpio_num_t ins[] = {NES_DATA_P1, NES_DATA_P2};
    for (gpio_num_t p : ins) {
        rtc_gpio_init(p);
        rtc_gpio_set_direction(p, RTC_GPIO_MODE_INPUT_ONLY);
        // Pull-UP here, deliberately *opposite* of the awake reader's pull-down (NESController::begin).
        // Asleep we only need wake-on-press: a deselected/high-Z line must read HIGH (released) so it
        // isn't read as "all buttons pressed" by button_pressed_in_last_read(), and so the CD4021-
        // driven idle line (also high) sinks no standby current. The player-select sentinel (which
        // needs the deselected line LOW) is only used while awake.
        rtc_gpio_pulldown_dis(p);
        rtc_gpio_pullup_en(p);
    }
}

void ulp_load() {
    config_rtc_gpio();
    std::memset(RTC_SLOW_MEM, 0, CONFIG_ULP_COPROC_RESERVE_MEM);

    const ulp_insn_t program[] = {
        // --- CD4021 parallel load: pulse LATCH high ~18us, then low ---
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kLatchBit, 1),
        I_DELAY(150),
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kLatchBit, 0),
        I_DELAY(60),

        // R2 = P1 byte, R3 = P2 byte, R1 = bit counter (8)
        I_MOVI(R2, 0),
        I_MOVI(R3, 0),
        I_MOVI(R1, 8),

        M_LABEL(1),                                       // for each of 8 bits:
        I_RD_REG(RTC_GPIO_IN_REG, kData1Bit, kData1Bit),  //   R0 = P1 data bit
        I_LSHI(R2, R2, 1),                                //   R2 = (R2 << 1) | bit  (A ends at MSB)
        I_ORR(R2, R2, R0),
        I_RD_REG(RTC_GPIO_IN_REG, kData2Bit, kData2Bit),  //   R0 = P2 data bit
        I_LSHI(R3, R3, 1),
        I_ORR(R3, R3, R0),
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kClk1Bit, 1),      //   clock pulse (both players together)
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kClk2Bit, 1),
        I_DELAY(60),
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kClk1Bit, 0),
        I_WR_REG_BIT(RTC_GPIO_OUT_REG, kClk2Bit, 0),
        I_DELAY(60),
        I_SUBI(R1, R1, 1),
        M_BXZ(2),                                         //   counter == 0 -> done
        M_BX(1),

        // --- Wake the SoC only if the state changed since the last stored read ---
        // The stored words (kAddrP1/P2) double as "previous state" and as what the CPU reads.
        // ulp_run()'s immediate run (SoC awake) seeds them from memset(0) -> a no-op I_WAKE, so
        // only real changes wake the SoC thereafter.
        M_LABEL(2),
        I_MOVI(R1, kAddrP1),             // R1 = base address (prev state == CPU-visible state)
        I_LD(R0, R1, 0),                 // R0 = prev P1
        I_SUBR(R0, R2, R0),              // R0 = newP1 - prevP1  (sets the ALU zero flag)
        M_BXZ(3),                        // P1 unchanged -> check P2
        M_BX(4),                         // P1 changed   -> wake
        M_LABEL(3),
        I_LD(R0, R1, 1),                 // R0 = prev P2
        I_SUBR(R0, R3, R0),              // R0 = newP2 - prevP2
        M_BXZ(5),                        // both unchanged -> halt without waking
        M_LABEL(4),                      // changed: publish new state + wake
        I_ST(R2, R1, 0),
        I_ST(R3, R1, 1),
        I_WAKE(),
        M_LABEL(5),
        I_HALT(),                        // halt until the next wakeup-timer tick
    };
    size_t size = sizeof(program) / sizeof(ulp_insn_t);
    ESP_ERROR_CHECK(ulp_process_macros_and_load(0, program, &size));
    ulp_set_wakeup_period(0, kUlpPeriodUs);
    ESP_ERROR_CHECK(ulp_run(0));
}

uint8_t last_p1() { return RTC_SLOW_MEM[kAddrP1] & 0xFF; }
uint8_t last_p2() { return RTC_SLOW_MEM[kAddrP2] & 0xFF; }

bool button_pressed_in_last_read(uint8_t bit) {
    if (bit > 7) return false;
    uint8_t mask = 0x80 >> bit;          // MSB-first: A B Sel Sta U D L R
    // Active-low: bit CLEAR = pressed. The deselected player's whole line reads all-high (released),
    // so a clear bit on either line means a real press on the active controller.
    bool p1 = (last_p1() & mask) == 0;
    bool p2 = (last_p2() & mask) == 0;
    return p1 || p2;
}

void release_after_wake() {
    // Undo the ULP's RTC-GPIO config so the normal gpio driver owns the NES pins again.
    const gpio_num_t pins[] = {NES_LATCH, NES_CLK_P1, NES_CLK_P2, NES_DATA_P1, NES_DATA_P2};
    for (gpio_num_t p : pins) {
        rtc_gpio_hold_dis(p);
        rtc_gpio_deinit(p);
    }
}

void deep_sleep() {
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
    esp_deep_sleep_start();   // does not return; the SoC reboots on wake
}

} // namespace power