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

    // Wiring diagnosis. A live, selected data line always carries at least one released button
    // (Up/Down and Left/Right are mutually exclusive, so it can never read all-pressed), so a line
    // that reads the all-pressed sentinel (0xFF) is deselected or disconnected. Both lines at 0xFF
    // means no controller is driving either line: nothing is wired to J2 (or both DATA lines are
    // broken). Toggling the player-select slider moves the live line, so P1-only / P2-only wiring
    // faults show as one side going live and the other staying at the sentinel.
    enum ControllerState : uint8_t {
        NES_OK_P1,       // P1 line live (P1 selected and connected)
        NES_OK_P2,       // P2 line live
        NES_OK_BOTH,     // both lines live
        NES_NO_SIGNAL,   // both lines idle-high: no controller detected on J2
    };

    NESController(gpio_num_t clkPin1, gpio_num_t clkPin2, gpio_num_t latchPin,
                  gpio_num_t dataPin1, gpio_num_t dataPin2);

    void begin();                                   // configure GPIOs
    void read();                                    // latch + clock out 8 bits for both players

    // Active raw sample of both DATA lines, packed bit7=A..bit0=R, 1=pressed (line low). A high-Z or
    // disconnected line reads 0xFF. Does not disturb read()/gameplay state, so it is safe to call
    // outside the main poll loop (e.g. the boot wiring diagnostic).
    void sampleRaw(uint8_t& p1, uint8_t& p2) const;

    // Sample a few times and classify which lines are live. Biased toward "present": reports
    // NES_NO_SIGNAL only when no line drives across the whole window.
    ControllerState diagnose(int samples = 6) const;

    static ControllerState classify(uint8_t p1, uint8_t p2) {
        bool p1live = p1 != 0xFF, p2live = p2 != 0xFF;
        if (p1live && p2live) return NES_OK_BOTH;
        if (p1live)           return NES_OK_P1;
        if (p2live)           return NES_OK_P2;
        return NES_NO_SIGNAL;
    }

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