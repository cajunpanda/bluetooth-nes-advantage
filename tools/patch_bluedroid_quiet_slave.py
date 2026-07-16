# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins

# PlatformIO pre-build patch: when a host pages US (we are the ACL slave), stay silent on the
# ACL data plane until the host authenticates us, like a real Switch Pro Controller.
# See docs/switch_pro_protocol.md and history/switch2-support.md.
#
# Why: stock Bluedroid sends an L2CAP Information Request (Extended Features) the moment any ACL
# comes up (l2c_link_hci_conn_comp, stack/l2cap/l2c_link.c) - including inbound, pre-authentication
# connections. The Switch 2's Grip-menu flow pages the controller and then expects a real Pro's
# behavior: total silence until the console drives Secure Simple Pairing. Bench contrast against
# the same console: a BlueZ host (nxbt) that stays passive gets the console's IO Capability
# exchange within ~2 ms of connect and pairs; this device, whose only unauthenticated ACL-data
# packet is that Information Request ~100 ms after connect, never sees SSP start and is dropped
# ~0.3 s later with HCI reason 0x05 (Authentication Failure).
#
# Fix: send the connection-time Extended Features probe only when we originated the link (ACL
# master). Skipping it entirely on slave links is safe for every profile this device runs: the
# HID channels use L2CAP basic mode, which l2c_fcr_chk_chan_modes() always permits when the peer's
# extended-feature mask was never fetched (mask 0 only strips ERTM/streaming), and with
# w4_info_rsp never set the channel state machine proceeds immediately on both the inbound
# (connect-ind) and outbound (connect-req) paths instead of stalling for the response.
# Board-originated links (the reconnect "kick" to 8BitDo-style hosts) keep stock behavior.
# Idempotent and safe to re-run.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os

idf = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
path = os.path.join(idf, "components", "bt", "host", "bluedroid", "stack", "l2cap", "l2c_link.c")

OLD = """        /* Get the peer information if the l2cap flow-control/rtrans is supported */
        l2cu_send_peer_info_req (p_lcb, L2CAP_EXTENDED_FEATURES_INFO_TYPE);"""

NEW = """        /* Get the peer information if the l2cap flow-control/rtrans is supported.
        ** btna: only when we originated the link. As slave (host paged us) stay silent on the
        ** data plane until authenticated, like a real HID peripheral - the Switch 2 drops
        ** pre-auth chatterers with reason 0x05 before ever starting SSP. Basic-mode channels
        ** never need the peer's extended-feature mask. */
        if (p_lcb->link_role == HCI_ROLE_MASTER) {
            l2cu_send_peer_info_req (p_lcb, L2CAP_EXTENDED_FEATURES_INFO_TYPE);
        }"""

if not os.path.isfile(path):
    print("[btna quiet-slave-patch] SKIP: not found: %s" % path)
else:
    with open(path) as f:
        src = f.read()
    if NEW in src:
        print("[btna quiet-slave-patch] already applied (no change)")
    elif OLD in src:
        src = src.replace(OLD, NEW, 1)
        with open(path, "w") as f:
            f.write(src)
        print("[btna quiet-slave-patch] patched l2c_link_hci_conn_comp: no pre-auth info req as slave")
    else:
        print("[btna quiet-slave-patch] WARNING: anchor not found in %s - IDF version changed?" % path)
