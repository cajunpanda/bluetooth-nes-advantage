# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins

# PlatformIO pre-build patch: allow enabling BR/EDR Secure Connections host support on ESP32.
# See docs/switch_pro_protocol.md and history/switch2-support.md.
#
# Why: the Switch 2 pages this controller (CoD gate solved), completes the ACL, then drops it
# with HCI reason 0x05 (Authentication Failure) ~60 ms after connect WITHOUT ever starting Secure
# Simple Pairing - no IO Capability exchange, no Link Key Request, nothing host-visible on our
# side. In that window the console can only have examined our LMP feature pages. Bench facts:
#   - Our extended features page 1 reads 0x07: SSP host support set, but "Secure Connections
#     (Host Support)" (bit 3) CLEAR. Page 2 shows the ESP32 controller itself DOES support SC.
#   - The proven-working reference (nxbt on BlueZ/Intel AX211, same console, same screen) has SC
#     host support set, and the console opens SSP within ~2 ms of connect.
# So the console appears to require SC host support on the legacy Pro-Controller pairing path,
# and rejects non-SC hosts pre-SSP with 0x05.
#
# The host stack has a runtime switch for this (esp_bluedroid_config_t.sc_en; bt_pro.cpp sets it),
# and Bluedroid's controller startup then issues HCI_Write_Secure_Connections_Host_Support when
# feature page 2 confirms controller support. But on the ESP32 target, bt_target.h hard-defines
# SC_MODE_INCLUDED FALSE ("Disable AES-CCM ... to workaround controller AES issue"; E0 fallback)
# and esp_bluedroid_init_with_cfg() then rejects sc_en=true with ESP_ERR_INVALID_ARG. That is a
# blanket precaution, not a per-connection failure we have observed; we accept the risk knowingly
# to reach the Switch 2's SSP at all. SC_MODE_INCLUDED gates nothing else in this IDF (6.0.1):
# its only uses are this define and that init guard.
#
# Fix: force SC_MODE_INCLUDED TRUE for the ESP32 target. Idempotent and safe to re-run.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os

idf = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
path = os.path.join(idf, "components", "bt", "host", "bluedroid", "common", "include", "common",
                    "bt_target.h")

OLD = """#define SC_MODE_INCLUDED                FALSE"""

NEW = """/* btna: was FALSE. SC host support enabled despite the stock AES-CCM workaround note above -
** the Switch 2 refuses to start SSP with a host that lacks Secure Connections host support,
** so without this the console drops us with reason 0x05 before pairing begins. */
#define SC_MODE_INCLUDED                TRUE"""

if not os.path.isfile(path):
    print("[btna sc-mode-patch] SKIP: not found: %s" % path)
else:
    with open(path) as f:
        src = f.read()
    if NEW in src:
        print("[btna sc-mode-patch] already applied (no change)")
    elif OLD in src:
        src = src.replace(OLD, NEW, 1)
        with open(path, "w") as f:
            f.write(src)
        print("[btna sc-mode-patch] patched bt_target.h: SC_MODE_INCLUDED TRUE on ESP32")
    else:
        print("[btna sc-mode-patch] WARNING: anchor not found in %s - IDF version changed?" % path)
