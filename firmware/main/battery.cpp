// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "battery.hpp"
#include "board_config.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "battery";

// Sense chain: VBAT -> R12/R13 (1M/1M) -> C8 (100 nF) -> GPIO34 (ADC1_CH6). VBAT = 2 * V(sense).
// BATT_CAL_PERMILLE trims the nominal /2 for 5% resistor tolerance + ADC gain error; set it from a
// bench reading (compare `adc`/`batt` against a meter on +BATT). 1000 = no trim.
#define BATT_ADC_CHANNEL    ADC_CHANNEL_6      // GPIO34, ADC1
#define BATT_ADC_ATTEN      ADC_ATTEN_DB_12    // ~0..3.1 V; divided 4.2 V cell reads ~2.1 V
#define BATT_DIVIDER_NUM    2                  // (R12 + R13) / R13
#define BATT_CAL_PERMILLE   987                // gain trim for divider + ADC error; 1000 = no trim
#define BATT_CHARGE_OFFSET_MV 55               // while charging, strip the charge-current IR rise
                                               // (~0.78 A Ichg * ~75 mohm cell ESR)
#define BATT_OVERSAMPLE     32
#define BATT_SETTLE_MS      300                 // ~6 RC of the 500k * 100 nF sense node
#define BATT_PRESENT_MV     2800                // below this, treat the cell as absent
#define BATT_POLL_MS        30000
#define BATT_EMA_NUM        1                   // EMA weight of a new sample: new/(new+old)
#define BATT_EMA_DEN        4

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t          s_cali = nullptr;   // raw -> mV; nullptr = mV unavailable

static bool     s_present  = false;
static uint8_t  s_pct      = 100;
static int      s_ema_mv   = -1;                       // EMA state on VBAT mV, -1 = unseeded

// Coarse 1S-LiPo resting SoC curve. Approximate under load; good enough for a status LED.
static uint8_t mv_to_pct(int mv) {
    static const struct { int mv; uint8_t pct; } lut[] = {
        {4200, 100}, {4100, 90}, {4000, 80}, {3900, 70}, {3800, 60}, {3700, 45},
        {3600, 30},  {3500, 18}, {3400, 10}, {3300, 5},  {3200, 2},  {3000, 0},
    };
    const size_t n = sizeof(lut) / sizeof(lut[0]);
    if (mv >= lut[0].mv)     return 100;
    if (mv <= lut[n - 1].mv) return 0;
    for (size_t i = 1; i < n; i++) {
        if (mv >= lut[i].mv) {
            int m0 = lut[i].mv,  m1 = lut[i - 1].mv;
            int p0 = lut[i].pct, p1 = lut[i - 1].pct;
            return (uint8_t)(p0 + (mv - m0) * (p1 - p0) / (m1 - m0));
        }
    }
    return 0;
}

int battery::read_raw() {
    if (!s_adc) return -1;
    gpio_set_level((gpio_num_t)BATT_EN, 1);            // ground the divider (Q2 on)
    vTaskDelay(pdMS_TO_TICKS(BATT_SETTLE_MS));         // let C8 settle to VBAT/2
    int acc = 0, n = 0;
    for (int i = 0; i < BATT_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) == ESP_OK) { acc += raw; n++; }
    }
    gpio_set_level((gpio_num_t)BATT_EN, 0);            // release (divider draws ~0)
    return n ? acc / n : -1;
}

int battery::read_sense_mv() {
    int raw = read_raw();
    if (raw < 0 || !s_cali) return -1;
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}

int battery::read_mv() {
    int sense = read_sense_mv();
    if (sense < 0) return -1;
    return sense * BATT_DIVIDER_NUM * BATT_CAL_PERMILLE / 1000;
}

// Background poll: settled VBAT read -> EMA -> presence + SoC. Charge-status pins are read directly
// on demand (no settle), so is_charging()/is_full() stay live between polls.
static void batt_task(void*) {
    for (;;) {
        int mv = battery::read_mv();
        if (mv > 0) {
            if (s_ema_mv < 0) s_ema_mv = mv;
            else s_ema_mv += (mv - s_ema_mv) * BATT_EMA_NUM / BATT_EMA_DEN;
            s_present = s_ema_mv >= BATT_PRESENT_MV;

            // SoC from the resting curve, corrected for the charging state. The terminal reads high
            // while charging (charge-current IR rise), so strip a fixed offset before the curve; and
            // never report 100% until STBY (is_full) actually confirms it - the elevated voltage
            // otherwise pins the curve at 100% mid-charge.
            bool chg = battery::is_charging(), full = battery::is_full();
            if (!s_present)   s_pct = 100;                       // absent: report full (timers behave)
            else if (full)    s_pct = 100;                       // STBY: truly full
            else {
                uint8_t p = mv_to_pct(chg ? s_ema_mv - BATT_CHARGE_OFFSET_MV : s_ema_mv);
                s_pct = (chg && p > 99) ? 99 : p;
            }
            ESP_LOGI(TAG, "VBAT=%d mV (ema %d) present=%d pct=%u chg=%d full=%d",
                     mv, s_ema_mv, s_present, s_pct, chg, full);
        } else {
            s_present = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BATT_POLL_MS));
    }
}

void battery::init() {
    // BATT_EN drives the divider ground switch; idle low so the divider draws ~0.
    gpio_config_t en = {};
    en.mode = GPIO_MODE_OUTPUT;
    en.pin_bit_mask = 1ULL << BATT_EN;
    gpio_config(&en);
    gpio_set_level((gpio_num_t)BATT_EN, 0);

    // TP4056 status: open-drain, active-low, so pull up and invert on read.
    gpio_config_t st = {};
    st.mode = GPIO_MODE_INPUT;
    st.pull_up_en = GPIO_PULLUP_ENABLE;
    st.pin_bit_mask = (1ULL << CHG_STAT) | (1ULL << STBY_STAT);
    gpio_config(&st);

    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 init failed; battery sense off");
        s_adc = nullptr;
        return;
    }
    adc_oneshot_chan_cfg_t chan = {};
    chan.atten = BATT_ADC_ATTEN;
    chan.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan);

    adc_cali_line_fitting_config_t cali = {};
    cali.unit_id = ADC_UNIT_1;
    cali.atten = BATT_ADC_ATTEN;
    cali.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 cali init failed; battery mV unavailable");
        s_cali = nullptr;
    }

    xTaskCreate(batt_task, "batt", 3072, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "battery monitor up (ADC1_CH6, /%d divider, %d ms settle)",
             BATT_DIVIDER_NUM, BATT_SETTLE_MS);
}

bool    battery::present()        { return s_present; }
uint8_t battery::level_percent()  { return s_pct; }
bool    battery::is_charging()    { return gpio_get_level((gpio_num_t)CHG_STAT)  == 0; }
bool    battery::is_full()        { return gpio_get_level((gpio_num_t)STBY_STAT) == 0; }
bool    battery::external_power() { return is_charging() || is_full(); }
