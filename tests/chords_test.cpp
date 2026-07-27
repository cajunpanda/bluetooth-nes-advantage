// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins
//
// Host-side tests for the chord layer (Select as a shift key).
//
// apply_chords() is static in app_main.cpp and sits behind ESP-IDF, so run.sh lifts the whole chord
// section out of that file and includes it below. The test therefore exercises the shipped state
// machine, not a transcription of it: change app_main.cpp and these tests see the change. The cost
// is the marker comments run.sh keys on, which must not be renamed without updating run.sh.
//
// Everything the section touches is stubbed here: a fake clock (so windows and pulses are exact
// rather than timing-dependent), the NesInput fields, the two settings constants, and ESP_LOGI.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static const char* TAG = "app";
static int64_t g_now = 0;                     // fake clock, advanced by the scenarios
static inline int64_t now_ms() { return g_now; }

static bool g_verbose = false;
#define ESP_LOGI(tag, ...) \
    do { if (g_verbose) { (void)(tag); printf("  [log] "); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace settings {
constexpr uint16_t kChordOff  = 0;
constexpr uint16_t kChordHold = 0xFFFF;
}

namespace bt {
struct NesInput {
    bool a = false, b = false, select = false, start = false;
    bool up = false, down = false, left = false, right = false;
    bool home = false, capture = false, zl = false, zr = false;
};
}

#include "chords_extracted.inc"

// ---- test scaffolding -------------------------------------------------------------------------
static int g_fail = 0;
static std::string g_case;

struct Out { bool minus, home, capture, zl, zr, up, down, left, right, start; };

// Run one poll: set raw buttons, resolve, return what the host would see.
static Out poll(bool select, bool start = false, bool up = false, bool down = false,
                bool left = false, bool right = false) {
    bt::NesInput in;
    in.select = select; in.start = start;
    in.up = up; in.down = down; in.left = left; in.right = right;
    apply_chords(in);
    return { in.select, in.home, in.capture, in.zl, in.zr,
             in.up, in.down, in.left, in.right, in.start };
}

static void advance(int64_t ms) { g_now += ms; }

static void begin(const char* name, uint16_t window) {
    g_case = name;
    s_chord_window = window;
    chords_reset();
    s_prev_members = 0;
    g_now = 100000;   // arbitrary non-zero base
}

static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL [%s] %s\n", g_case.c_str(), what); g_fail++; }
}

// ---- scenarios --------------------------------------------------------------------------------
int main(int argc, char** argv) {
    g_verbose = (argc > 1 && !strcmp(argv[1], "-v"));

    // --- OFF: Select is an ordinary button -----------------------------------------------------
    {
        begin("off/plain-press", settings::kChordOff);
        check(poll(true).minus,  "Select down -> Minus immediately");
        advance(500);
        check(poll(true).minus,  "Minus still held after 500 ms");
        check(!poll(false).minus, "Minus released with Select");

        begin("off/no-chords", settings::kChordOff);
        poll(true);
        advance(10);
        Out o = poll(true, false, false, true);   // Select + Down
        check(o.minus && o.down, "Select+Down stays Minus+Down, no Capture");
        check(!o.capture, "no Capture fired with chords off");
    }

    // --- HOLD: 2.2.x behaviour ------------------------------------------------------------------
    {
        begin("hold/minus-on-release", settings::kChordHold);
        check(!poll(true).minus, "Select down -> Minus withheld");
        advance(2000);
        check(!poll(true).minus, "still withheld after 2 s (no limit)");
        Out o = poll(false);
        check(o.minus, "Minus pulses on release");
        advance(50);
        check(poll(false).minus, "pulse still high at 50 ms");
        advance(40);
        check(!poll(false).minus, "pulse ended by 90 ms");

        begin("hold/late-chord", settings::kChordHold);
        poll(true);
        advance(1500);                            // way past any bounded window
        Out o2 = poll(true, false, false, true);   // Select + Down
        check(o2.capture && !o2.down && !o2.minus, "late Select+Down still chords to Capture");
        Out o3 = poll(false);
        check(!o3.minus, "no Minus pulse after a chord");
    }

    // --- BOUNDED: chord inside the window -------------------------------------------------------
    {
        begin("200/chord-inside", 200);
        check(!poll(true).minus, "Select down -> withheld while undecided");
        advance(120);
        Out o = poll(true, false, false, true);
        check(o.capture && !o.down && !o.minus, "Down at 120 ms chords to Capture");
        advance(1000);
        o = poll(true, false, false, true);
        check(o.capture && !o.minus, "chord output follows the hold past the deadline");
        check(!poll(false).minus, "no Minus pulse after a chord");
    }

    // --- BOUNDED: window closes, Minus commits and is holdable ----------------------------------
    {
        begin("200/commit-and-hold", 200);
        check(!poll(true).minus, "withheld at t=0");
        advance(150);
        check(!poll(true).minus, "still withheld at 150 ms");
        advance(60);                                    // t = 210 ms
        check(poll(true).minus, "Minus commits once the window closes");
        advance(3000);
        check(poll(true).minus, "Minus stays held for as long as Select is (the customer's fix)");
        check(!poll(false).minus, "Minus releases with Select");
        advance(10);
        check(!poll(false).minus, "no extra pulse after a committed Minus");
    }

    // --- BOUNDED: late member does NOT chord ----------------------------------------------------
    {
        begin("200/late-member-no-chord", 200);
        poll(true);
        advance(400);                                   // window long closed
        Out o = poll(true, false, false, true);         // Select + Down
        check(o.minus && o.down, "after the window, Select+Down is Minus+Down");
        check(!o.capture, "no Capture from a late member");
    }

    // --- BOUNDED: quick tap still pulses on release ----------------------------------------------
    {
        begin("200/quick-tap", 200);
        poll(true);
        advance(40);
        check(!poll(true).minus, "40 ms in, still undecided");
        Out o = poll(false);                            // released inside the window
        check(o.minus, "a tap shorter than the window pulses on release");
    }

    // --- Pre-existing direction is not chordable (regression from 35753b5) -----------------------
    {
        begin("200/held-direction-passthru", 200);
        poll(false, false, false, false, false, true);  // running right, no Select
        advance(10);
        Out o = poll(true, false, false, false, false, true);   // Select lands while Right held
        check(o.right && !o.zr, "already-held Right keeps steering, does not chord to ZR");
        advance(300);
        o = poll(true, false, false, false, false, true);
        check(o.minus && o.right, "window closes -> Minus + Right, still no ZR");
    }

    // --- Same-poll member still chords ----------------------------------------------------------
    {
        begin("200/same-poll-chord", 200);
        Out o = poll(true, false, false, false, false, true);   // Select + Right in one poll
        check(o.zr && !o.right && !o.minus, "member landing with Select chords to ZR");
    }

    // --- Two taps must not produce a phantom chord (regression from 35753b5) ---------------------
    {
        begin("200/double-tap-no-phantom", 200);
        poll(false, false, false, false, false, true);          // Right held
        advance(10);
        poll(true, false, false, false, false, true);           // tap 1
        advance(20);
        poll(false, false, false, false, false, true);
        advance(20);
        Out o = poll(true, false, false, false, false, true);   // tap 2, Right still held
        check(!o.zr, "second tap with Right still held does not phantom-chord to ZR");
    }

    // --- Select+Start -> Home inside the window -------------------------------------------------
    {
        begin("200/home", 200);
        poll(true);
        advance(50);
        Out o = poll(true, true);
        check(o.home && !o.start && !o.minus, "Select+Start chords to Home");
    }

    // --- Up -> ZL+ZR ----------------------------------------------------------------------------
    {
        begin("200/zl-zr", 200);
        Out o = poll(true, false, true);
        check(o.zl && o.zr && !o.up, "Select+Up chords to ZL+ZR");
    }

    // --- Re-arm inside the pulse window truncates it ---------------------------------------------
    {
        begin("200/rearm-truncates-pulse", 200);
        poll(true);
        advance(30);
        check(poll(false).minus, "tap pulses on release");
        advance(10);
        Out o = poll(true);                             // re-press inside the 80 ms pulse
        check(!o.minus, "re-arming truncates the pulse rather than leaking Minus into the new press");
    }

    if (g_fail == 0) printf("  ok: all chord scenarios passed\n");
    else             printf("  %d assertion(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
