// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "nes_controller.hpp"
#include "esp_timer.h"
#include "esp_rom_sys.h"          // esp_rom_delay_us
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline int64_t now_ms() { return esp_timer_get_time() / 1000; }

NESController::NESController(gpio_num_t clkPin1, gpio_num_t clkPin2, gpio_num_t latchPin,
                            gpio_num_t dataPin1, gpio_num_t dataPin2)
    : _clk1(clkPin1), _clk2(clkPin2), _latch(latchPin), _data1(dataPin1), _data2(dataPin2),
      _stateChanged{false, false}, _playerSelection(0), _lastActivityMs(0), _cb(nullptr) {
    for (int p = 0; p < 2; p++)
        for (int b = 0; b < 8; b++)
            _buttonState[p][b] = _prevButtonState[p][b] = false;
}

void NESController::begin() {
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << _clk1) | (1ULL << _clk2) | (1ULL << _latch);
    gpio_config(&out);

    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    // Pull-DOWN, not pull-up: the CD4021 push-pull output drives the selected line correctly either
    // way, but a deselected (high-Z) line must read LOW so it shifts out all-1s (active-low invert),
    // the "all-true" player-select sentinel. The deep-sleep ULP path uses pull-UP instead (opposite
    // need; see power.cpp config_rtc_gpio).
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;
    in.pin_bit_mask = (1ULL << _data1) | (1ULL << _data2);
    gpio_config(&in);

    gpio_set_level(_latch, 0);
    gpio_set_level(_clk1, 0);
    gpio_set_level(_clk2, 0);
    _lastActivityMs = now_ms();
}

void NESController::read() {
    _stateChanged[0] = _stateChanged[1] = false;

    // Latch current button states (CD4021 parallel load; min ~12 us)
    gpio_set_level(_latch, 1);
    esp_rom_delay_us(12);
    gpio_set_level(_latch, 0);

    // Clock out all 8 buttons (active-low, invert)
    for (int i = 0; i < 8; i++) {
        _buttonState[0][i] = !gpio_get_level(_data1);
        _buttonState[1][i] = !gpio_get_level(_data2);

        gpio_set_level(_clk1, 1);
        gpio_set_level(_clk2, 1);
        esp_rom_delay_us(6);
        gpio_set_level(_clk1, 0);
        gpio_set_level(_clk2, 0);
        esp_rom_delay_us(6);
    }

    // Detect changes
    for (int i = 0; i < 8; i++) {
        if (_buttonState[0][i] != _prevButtonState[0][i]) {
            _stateChanged[0] = true;
            _prevButtonState[0][i] = _buttonState[0][i];
        }
        if (_buttonState[1][i] != _prevButtonState[1][i]) {
            _stateChanged[1] = true;
            _prevButtonState[1][i] = _buttonState[1][i];
        }
    }

    // Player-select switch: the deselected player's line reads all-8-high.
    uint8_t previous = _playerSelection;
    if (allTrue(_buttonState[0], 8) && _playerSelection != 1) {
        _playerSelection = 1;                  // P2 selected
    } else if (allTrue(_buttonState[1], 8) && _playerSelection != 0) {
        _playerSelection = 0;                  // P1 selected
    }
    if (_playerSelection != previous) {
        vTaskDelay(pdMS_TO_TICKS(100));        // debounce the switch
        if (_cb) _cb(_playerSelection);
    }

    if (_stateChanged[0] || _stateChanged[1]) _lastActivityMs = now_ms();
}

void NESController::sampleRaw(uint8_t& p1, uint8_t& p2) const {
    p1 = p2 = 0;
    gpio_set_level(_latch, 1);
    esp_rom_delay_us(12);
    gpio_set_level(_latch, 0);
    for (int i = 0; i < 8; i++) {
        p1 = (p1 << 1) | (gpio_get_level(_data1) ? 0 : 1);   // active-low: line low = pressed
        p2 = (p2 << 1) | (gpio_get_level(_data2) ? 0 : 1);
        gpio_set_level(_clk1, 1);
        gpio_set_level(_clk2, 1);
        esp_rom_delay_us(6);
        gpio_set_level(_clk1, 0);
        gpio_set_level(_clk2, 0);
        esp_rom_delay_us(6);
    }
}

NESController::ControllerState NESController::diagnose(int samples) const {
    bool p1live = false, p2live = false;
    for (int i = 0; i < samples; i++) {
        uint8_t a, b;
        sampleRaw(a, b);
        if (a != 0xFF) p1live = true;
        if (b != 0xFF) p2live = true;
    }
    return classify(p1live ? 0x00 : 0xFF, p2live ? 0x00 : 0xFF);
}

bool NESController::stateChanged(uint8_t player) const {
    return (player < 2) ? _stateChanged[player] : false;
}

bool NESController::getButtonState(uint8_t player, uint8_t button) const {
    return (player < 2 && button < 8) ? _buttonState[player][button] : false;
}

void NESController::resetLastActivity() { _lastActivityMs = now_ms(); }

uint8_t NESController::getHatDirection(uint8_t player) const {
    if (player >= 2) return 0;
    const bool* b = _buttonState[player];
    if (b[BUTTON_UP] && b[BUTTON_RIGHT])    return 2;
    if (b[BUTTON_RIGHT] && b[BUTTON_DOWN])  return 4;
    if (b[BUTTON_DOWN] && b[BUTTON_LEFT])   return 6;
    if (b[BUTTON_LEFT] && b[BUTTON_UP])     return 8;
    if (b[BUTTON_UP])    return 1;
    if (b[BUTTON_RIGHT]) return 3;
    if (b[BUTTON_DOWN])  return 5;
    if (b[BUTTON_LEFT])  return 7;
    return 0;
}

int8_t NESController::getXAxis(uint8_t player) const {
    if (player >= 2) return 0;
    return _buttonState[player][BUTTON_RIGHT] ? 127
         : (_buttonState[player][BUTTON_LEFT] ? -127 : 0);
}

int8_t NESController::getYAxis(uint8_t player) const {
    if (player >= 2) return 0;
    return _buttonState[player][BUTTON_DOWN] ? 127
         : (_buttonState[player][BUTTON_UP] ? -127 : 0);
}

bool NESController::allTrue(const bool a[], int n) {
    for (int i = 0; i < n; i++) if (!a[i]) return false;
    return true;
}