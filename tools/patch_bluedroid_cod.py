# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins

# PlatformIO pre-build patch: let the device present a real Pro Controller's full Class of
# Device (0x002508) while general-discoverable. See docs/switch_pro_protocol.md.
#
# Why: the Switch 2's "Change Grip/Order" screen pages a controller only if its inquiry-response
# CoD matches a real Pro's 0x002508, which includes the "Limited Discoverable" service-class bit
# (0x2000). Bench A/B against a Switch 2: a BlueZ host presenting 0x002508 was paged within 2 s of
# the console entering the screen; this device presenting 0x000508 (same name/EIR/UUIDs, clean
# fingerprint) was never paged. The Switch 1 accepts either CoD.
#
# Two stock behaviors combine to make 0x002508 unreachable without a patch:
#   1. BTM_SetDiscoverability(GENERAL) (stack/btm/btm_inq.c) force-clears the limited-discoverable
#      CoD bit on every scan-mode change - Bluedroid derives that bit from the GAP mode instead of
#      treating it as part of the application's CoD. (Limited mode is no alternative: ESP32
#      Bluedroid then answers only the limited inquiry code, and the consoles inquire with the
#      general code.)
#   2. The ESP32 controller latches the CoD into its inquiry-response state when inquiry scan is
#      enabled: a Write_Class_of_Device issued while scan is already enabled returns success but
#      the FHS/EIR inquiry response keeps answering with the old CoD (verified via the hcilog HCI
#      ring: 0x002508 write + Command Complete success, radio still answering 0x000508). So the
#      strip in (1), which runs immediately before the scan-enable write, always wins.
#
# Fix: make the strip additive-only - entering limited mode may still set the bit, but general
# mode no longer clears it. The application then owns the bit via esp_bt_gap_set_cod(): bt_pro.cpp
# sets the full 0x002508 before HID startup enables inquiry scan, so the controller latches the
# real-Pro CoD. Idempotent and safe to re-run.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os

idf = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
path = os.path.join(idf, "components", "bt", "host", "bluedroid", "stack", "btm", "btm_inq.c")

OLD = "if (is_limited ^ cod_limited) {"
NEW = "if (is_limited && !cod_limited) { /* btna: additive only - general mode keeps the app's CoD bit */"

if not os.path.isfile(path):
    print("[btna cod-patch] SKIP: not found: %s" % path)
else:
    with open(path) as f:
        src = f.read()
    if NEW in src:
        print("[btna cod-patch] already applied (no change)")
    elif OLD in src:
        src = src.replace(OLD, NEW, 1)
        with open(path, "w") as f:
            f.write(src)
        print("[btna cod-patch] patched BTM_SetDiscoverability: general mode keeps CoD service bit")
    else:
        print("[btna cod-patch] WARNING: anchor not found in %s - IDF version changed?" % path)
