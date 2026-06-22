# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins

# PlatformIO pre-build patch: make the Bluedroid HID device behave like a real Switch Pro
# Controller at the link layer: a Bluetooth slave that never initiates sniff mode, but still
# accepts master-driven sniff. See docs/switch_pro_protocol.md.
#
# Why: stock Bluedroid's HID-device power spec slave-initiates sniff ~5s after connect (and again
# on idle). A real Pro Controller never does this; it lets the master (Switch / 8BitDo / PC) drive
# power management. The Switch tolerates our initiation; the 8BitDo NES receiver refuses it (HCI
# status 0x24, "LMP PDU not allowed") and BTA's retry storm drops the link every ~15s, so the
# receiver never holds a stable connection.
#
# Fix: in the HID-device (HD) entry of bta_dm_pm_spec[] (components/.../bta/dm/bta_dm_cfg.c), null
# the two timer-based sniff initiations (conn-open and idle) to BTA_DM_PM_NO_ACTION. The allow-mask
# (BTA_DM_PM_SNIFF | BTA_DM_PM_PARK) is left intact, so we still accept a master's sniff, exactly
# like a real Pro. The config array is `const`, so this can't be done at runtime; we patch the
# framework source at build time instead. Idempotent and safe to re-run.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os
import re

idf = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
path = os.path.join(idf, "components", "bt", "host", "bluedroid", "bta", "dm", "bta_dm_cfg.c")

if not os.path.isfile(path):
    print("[btna sniff-patch] SKIP: not found: %s" % path)
else:
    with open(path) as f:
        src = f.read()
    orig = src
    # Null all three sniff initiations in the HID-device (HD) spec: conn-open and idle (5000ms
    # timers) and "busy" (SNIFF_HD_ACTIVE @ 0, fired by our 60Hz report stream). BTA_DM_PM_SNIFF_HD_*
    # tokens are HD-specific, so these only touch the HID-device entry.
    src = re.sub(r"\{BTA_DM_PM_SNIFF_HD_ACTIVE_IDX, 5000 \+ BTA_DM_PM_SPEC_TO_OFFSET\}",
                 "{BTA_DM_PM_NO_ACTION, 0}", src)   # conn open
    src = re.sub(r"\{BTA_DM_PM_SNIFF_HD_IDLE_IDX,\s*5000 \+ BTA_DM_PM_SPEC_TO_OFFSET\}",
                 "{BTA_DM_PM_NO_ACTION, 0}", src)   # idle
    src = re.sub(r"\{BTA_DM_PM_SNIFF_HD_ACTIVE_IDX, 0\}",
                 "{BTA_DM_PM_NO_ACTION, 0}", src)   # busy
    if src != orig:
        with open(path, "w") as f:
            f.write(src)
        print("[btna sniff-patch] patched HID-device PM spec: no slave-initiated sniff")
    else:
        print("[btna sniff-patch] already applied (no change)")