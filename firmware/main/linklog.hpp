// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#pragma once

#include <cstdint>
#include <cstddef>

// Black box for pairing and reconnect faults. Always recording rather than a mode to switch on: a
// failed reconnect happens at power-on, before anything could be armed, and reading it out means
// the config-mode gesture, which reboots. So the ring lives in RTC_NOINIT memory, which survives
// esp_restart and deep sleep, and a per-boot summary goes to NVS so a power-off still leaves a
// trail. See docs/FIRMWARE.md "Link log".
//
// Cost is bounded: the ring is fixed-size RTC memory, and persist() writes at most a handful of
// times per boot into the partition that also holds the BTstack link keys.
namespace linklog {

// Event codes. `a`/`b` carry the HCI status/reason bytes that discriminate one failure from
// another; keep them raw, decoding belongs in the dump.
enum Ev : uint8_t {
    EV_BOOT = 1,     // a = esp_reset_reason(), b = transport (0 classic, 1 ble)
    EV_BONDS,        // a = stored link keys found at boot
    EV_PAGE,         // a = attempt number, b = 1 if this is a slow re-page
    EV_PAGE_ST,      // a = hid_device_connect() status
    EV_ACL,          // a = HCI connection complete status
    EV_AUTH,         // a = authentication complete status
    EV_KEY_REQ,      // the host asked us for the stored link key
    EV_KEY_NEW,      // a = key type; the host stored a NEW key, so it re-paired instead of resuming
    EV_PIN_REQ,      // host fell back to legacy PIN pairing
    EV_ENC,          // a = encryption change status, b = enabled
    EV_SSP,          // a = simple pairing complete status
    EV_HID_OPEN,     // a = 1 host-initiated, 0 device-initiated
    EV_HID_FAIL,     // a = status
    EV_HID_CLOSE,
    EV_DISC,         // a = HCI disconnection reason
    EV_SLOW,         // fast kick budget spent, falling back to slow re-page
    EV_GIVEUP,       // a = fast pages, b = slow pages
    EV_CONFIG,       // entered config / OTA mode
};

// Validate (or roll) the RTC ring and open the NVS history. Call once per boot, before Bluetooth.
void init(uint8_t transport);

void event(uint8_t ev, uint8_t a = 0, uint8_t b = 0);
void set_peer(const uint8_t addr[6]);

// Flush this boot's summary to NVS. Call at outcomes worth keeping across a power-off (HID up, HID
// gone, pages exhausted, sleep, reboot into config mode), not per event: writes are capped per boot
// and land next to the bonds.
void persist();

// printf-shaped sink, so both consoles pass what they already have: con_out over BLE, a vprintf
// wrapper on the wire.
using Out = void (*)(const char* fmt, ...);

size_t line_count();
// One logical line at a time, so the BLE console can pace a dump against its 3 KB log ring instead
// of overrunning it. The wired console walks 0..line_count().
void emit_line(size_t i, Out out);

}  // namespace linklog
