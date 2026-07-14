# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins

# PlatformIO pre-build patch set: make the Bluedroid Classic HID device interoperate with hosts
# that treat us exactly like a real Switch Pro Controller (8BitDo USB Adapter 2, BlueRetro).
# Companion to patch_bluedroid_sniff.py; see docs/switch_pro_protocol.md "Connection direction".
#
# Three targeted fixes, all in stack sources with no runtime hook (hence build-time patching):
#
# 1. hidd_conn.c hidd_l2cif_connect_ind: accept an incoming HID CTRL channel directly instead of
#    running btm_sec_mx_access_request. SSP has already authenticated and encrypted the link before
#    any L2CAP reaches us, but the btm gate re-authenticates the encrypted link (fresh Just-Works
#    keys race the security-flag update), and the ESP32 controller's LMP encryption pause/resume
#    wedges on that: ACL TX freezes (no Number-of-Completed-Packets ever again, no Encryption Key
#    Refresh Complete; seen live against the 8BitDo USB Adapter 2, plus a controller
#    ASSERT_WARN lc_task.c 1556 crash), the host never sees our ConnectRsp/ConfigReq and both
#    sides deadlock until their 30 s timers fire.
#
# 2. hidd_conn.c hidd_conn_initiate: complete a half-open HID connection. Hosts that mimic the
#    console's reconnect role open only CTRL and wait for the controller to open INTR (a real Pro
#    does exactly that); stock Bluedroid only self-originates INTR when it originated CTRL, so the
#    two sides deadlock. Also re-send the CTRL ConfigReq if config never completed (belt and
#    braces for hosts that missed a racy first ConfigReq). The app triggers this path via
#    esp_bt_hid_device_connect() after a grace window (bt_pro.cpp arm_hid_kick), so hosts that
#    open both channels themselves (Switch console, 8BitDo Retro Receiver) are unaffected.
#
# 3. bt_target.h HID_DEV_MTU_SIZE 64 -> 640: both HID channels config with MTU 640 like a real
#    Pro Controller.
#
# Each sub-patch is independent, idempotent, and safe to re-run.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os

idf = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
BLUEDROID = os.path.join(idf, "components", "bt", "host", "bluedroid")

HIDD_CONN = os.path.join(BLUEDROID, "stack", "hid", "hidd_conn.c")
BT_TARGET = os.path.join(BLUEDROID, "common", "include", "common", "bt_target.h")
BTA_HD_ACT = os.path.join(BLUEDROID, "bta", "hd", "bta_hd_act.c")

SEC_ANCHOR = """        p_hcon->conn_state = HID_CONN_STATE_SECURITY;
        if (btm_sec_mx_access_request(p_dev->addr, HID_PSM_CONTROL, FALSE, BTM_SEC_PROTO_HID, HIDD_NOSEC_CHN,
                                      &hidd_sec_check_complete, p_dev) == BTM_CMD_STARTED) {
            L2CA_ConnectRsp(bd_addr, id, cid, L2CAP_CONN_PENDING, L2CAP_CONN_OK);
        }
        return;"""

SEC_REPLACEMENT = """        p_hcon->conn_state = HID_CONN_STATE_SECURITY;
        /* btna hid-sec patch: SSP already authenticated + encrypted this link before any L2CAP
         * reached us. The btm_sec gate re-authenticates the encrypted link, which wedges the
         * ESP32 controller's LMP encryption pause/resume (ACL TX freeze), so accept directly. */
        hidd_sec_check_complete(p_dev->addr, BT_TRANSPORT_BR_EDR, p_dev, BTM_SUCCESS);
        return;"""

INITIATE_ANCHOR = """    if (p_dev->conn.conn_state != HID_CONN_STATE_UNUSED) {
        HIDD_TRACE_WARNING("%s: connection already in progress", __func__);
        return (HID_ERR_CONN_IN_PROCESS);
    }"""

INITIATE_REPLACEMENT = """    if (p_dev->conn.conn_state != HID_CONN_STATE_UNUSED) {
        /* btna hid-intr patch: the host opened CTRL but never INTR (8BitDo USB Adapter 2,
         * BlueRetro - hosts that expect the controller to open INTR like a real Pro Controller).
         * Two sub-cases:
         *  1. CTRL config incomplete: the host may have missed a ConfigReq sent while the link
         *     was not yet settled - re-send it.
         *  2. CTRL configured, INTR missing: originate INTR ourselves; IS_ORIG makes
         *     hidd_l2cif_connect_cfm accept the confirm and walk CONFIG -> CONNECTED as usual. */
        if (p_dev->conn.conn_state == HID_CONN_STATE_CONNECTING_INTR &&
            p_dev->conn.ctrl_cid != 0 && p_dev->conn.intr_cid == 0) {
            if ((p_dev->conn.conn_flags &
                 (HID_CONN_FLAGS_MY_CTRL_CFG_DONE | HID_CONN_FLAGS_HIS_CTRL_CFG_DONE)) !=
                (HID_CONN_FLAGS_MY_CTRL_CFG_DONE | HID_CONN_FLAGS_HIS_CTRL_CFG_DONE)) {
                HIDD_TRACE_WARNING("%s: half-open, CTRL config incomplete (flags 0x%02x), re-sending ConfigReq",
                                   __func__, p_dev->conn.conn_flags);
                L2CA_ConfigReq(p_dev->conn.ctrl_cid, &hd_cb.l2cap_cfg);
                return (HID_SUCCESS);
            }
            HIDD_TRACE_WARNING("%s: half-open (CTRL configured), originating INTR", __func__);
            p_dev->conn.conn_flags |= HID_CONN_FLAGS_IS_ORIG;
            p_dev->conn.disc_reason = HID_L2CAP_CONN_FAIL;
            if ((p_dev->conn.intr_cid = L2CA_ConnectReq(HID_PSM_INTERRUPT, p_dev->addr)) == 0) {
                HIDD_TRACE_WARNING("%s: could not start L2CAP connection for INTR", __func__);
                p_dev->conn.conn_flags &= ~HID_CONN_FLAGS_IS_ORIG;
                return (HID_ERR_L2CAP_FAILED);
            }
            return (HID_SUCCESS);
        }
        HIDD_TRACE_WARNING("%s: connection already in progress", __func__);
        return (HID_ERR_CONN_IN_PROCESS);
    }"""

MTU_ANCHOR = "#define HID_DEV_MTU_SIZE 64\n"
MTU_REPLACEMENT = "#define HID_DEV_MTU_SIZE 640 /* btna hid-intr patch: match real Pro Controller */\n"

# Registering the HID service with AUTHENTICATE|ENCRYPT makes btm add an implicit MITM requirement
# in SSP mode. A Just-Works pairing yields an unauthenticated link key, so when a host advertises
# IO caps that make an authenticated key theoretically possible (8BitDo USB Adapter 2 does; a
# Switch console reports NoInputNoOutput and doesn't), btm_sec_check_upgrade deletes the fresh key
# and re-pairs on the already-encrypted link - which the ESP32 controller's LMP encryption
# pause/resume cannot survive (ACL TX freeze). Real Pro Controllers use Just-Works keys with no
# MITM, so register with no channel-level security; SSP still authenticates + encrypts the link.
SEC_LVL_ANCHOR = "    HID_DevSetSecurityLevel(BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);"
SEC_LVL_REPLACEMENT = "    HID_DevSetSecurityLevel(BTA_SEC_NONE); /* btna hid-sec patch: no MITM upgrade, see tools/patch_bluedroid_hid_intr.py */"

PATCHES = [
    ("hid-sec: accept incoming CTRL without btm re-auth", HIDD_CONN, SEC_ANCHOR, SEC_REPLACEMENT),
    ("hid-intr: complete half-open HID connection",       HIDD_CONN, INITIATE_ANCHOR, INITIATE_REPLACEMENT),
    ("hid-mtu: HID_DEV_MTU_SIZE 64 -> 640",               BT_TARGET, MTU_ANCHOR, MTU_REPLACEMENT),
    ("hid-sec-lvl: HID service security NONE (no MITM key upgrade)", BTA_HD_ACT, SEC_LVL_ANCHOR, SEC_LVL_REPLACEMENT),
]

for name, path, anchor, replacement in PATCHES:
    if not os.path.isfile(path):
        print("[btna hid patches] SKIP (%s): not found: %s" % (name, path))
        continue
    with open(path) as f:
        src = f.read()
    if replacement in src:
        print("[btna hid patches] already applied: %s" % name)
    elif anchor in src:
        with open(path, "w") as f:
            f.write(src.replace(anchor, replacement, 1))
        print("[btna hid patches] applied: %s" % name)
    else:
        print("[btna hid patches] WARNING (%s): anchor not found in %s; framework changed?" % (name, path))
