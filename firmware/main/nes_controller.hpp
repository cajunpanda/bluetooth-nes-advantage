// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once
#include <cstdint>
#include "driver/gpio.h"

// Reads two NES Advantage controllers (P1/P2) that share LATCH/CLK via the CD4021 shift-register
// protocol, with separate DATA lines. Detects the player-select switch, which shorts all 8 buttons
// high on the deselected player's data line (the "all-true" sentinel). Buttons are active-low.

typedef void (*PlayerSelectionCallback)(uint8_t newPlayer);

class NESController {
public:
    // Button indices in NES poll order
    static constexpr uint8_t BUTTON_A      = 0;
    static constexpr uint8_t BUTTON_B      = 1;
    static constexpr uint8_t BUTTON_SELECT = 2;
    static constexpr uint8_t BUTTON_START  = 3;
    static constexpr uint8_t BUTTON_UP     = 4;
    static constexpr uint8_t BUTTON_DOWN   = 5;
    static constexpr uint8_t BUTTON_LEFT   = 6;
    static constexpr uint8_t BUTTON_RIGHT  = 7;

    NESController(gpio_num_t clkPin1, gpio_num_t clkPin2, gpio_num_t latchPin,
                  gpio_num_t dataPin1, gpio_num_t dataPin2);

    void begin();                                   // configure GPIOs
    void read();                                    // latch + clock out 8 bits for both players

    bool stateChanged(uint8_t player) const;
    bool getButtonState(uint8_t player, uint8_t button) const;
    uint8_t getPlayerSelection() const { return _playerSelection; }
    void setPlayerSelectionCallback(PlayerSelectionCallback cb) { _cb = cb; }

    int64_t getLastActivityMs() const { return _lastActivityMs; }
    void resetLastActivity();

    uint8_t getHatDirection(uint8_t player) const;  // 0=center, 1=up..8=up-left
    int8_t  getXAxis(uint8_t player) const;         // -127 / 0 / 127
    int8_t  getYAxis(uint8_t player) const;

private:
    gpio_num_t _clk1, _clk2, _latch, _data1, _data2;
    bool _buttonState[2][8];
    bool _prevButtonState[2][8];
    bool _stateChanged[2];
    uint8_t _playerSelection;       // 0 = P1, 1 = P2
    int64_t _lastActivityMs;
    PlayerSelectionCallback _cb;

    static bool allTrue(const bool a[], int n);
};