// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// Classic transport: Nintendo Switch Pro Controller emulation over BT Classic (Bluedroid esp_hidd).
// Wraps the bring-up, report descriptor, and 0x21/0x30 subcommand handshake in the bt::Ops transport
// interface:
//   * profiles and directional modes (NES buttons to Pro buttons / d-pad / left stick),
//   * the connection state machine: reconnect vs fresh-pair on boot, and identity/MAC refresh on
//     forget (forget drops bonds, bumps the identity generation, and reboots, so a forgotten host
//     cannot silently auto-reconnect to a stale BT address and skip the handshake).
//
// See docs/switch_pro_protocol.md for the wire format. The Switch identifies us by VID/PID, name,
// and CoD and drives everything over the subcommand handshake; the report descriptor only has to
// parse.

#include "bt_transport.hpp"
#include "settings.hpp"

#include <cinttypes>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "esp_timer.h"
#if defined(CONFIG_BT_SDP_COMMON_ENABLED)
#include "esp_sdp_api.h"
#endif

static const char* TAG = "bt_pro";

namespace {

// --- Internal Switch Pro state snapshot (built from NesInput + profile + directional mode) ------
struct ProState {
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false, zl = false, zr = false;
    bool minus = false, plus = false, home = false, capture = false;
    bool l_stick = false, r_stick = false;
    bool up = false, down = false, left = false, right = false;
    int16_t lx = 0, ly = 0;        // left stick, -1..+1 scaled (0 = centered)
};

esp_hidd_dev_t* s_dev = nullptr;
volatile bool s_connected = false;
volatile bt::LinkState s_link = bt::LINK_IDLE;
ProState s_state;                  // latest input snapshot
TaskHandle_t s_stream_task = nullptr;   // notified on input change so 0x30 ships immediately
uint8_t  s_battery = 0x80;         // Pro battery nibble<<4 | charge bits; 0x80 = full, not charging
esp_bd_addr_t s_peer = {0};        // connected host address (from GAP auth); used for QoS
esp_bd_addr_t s_bonded = {0};      // a stored host to actively page on boot (real-Pro reconnect)
bool s_have_bond = false;          // true means this boot is a reconnect, not a fresh pair

extern "C" esp_err_t esp_bt_hid_device_connect(esp_bd_addr_t bd_addr);
extern "C" esp_err_t esp_bt_hid_device_virtual_cable_unplug(void);

constexpr uint32_t kQosTpoll = 0x10;   // ~10 ms; pins the ACL link active (out of sniff)

// --- Device-initiated HID connect ("kick") -------------------------------------------------------
// A real Pro Controller advertises HIDReconnectInitiate and opens the HID L2CAP channels itself.
// Hosts that mimic the console's reconnect role (8BitDo USB Adapter 2, BlueRetro) therefore pair at
// GAP level and then wait forever for us to connect; only the Switch's Change Grip/Order screen and
// the older 8BitDo Retro Receiver initiate from their side. So: stay passive for a grace window
// (host-initiated still wins), then open the channels ourselves, with bounded retries so
// a dead host can't pin the radio awake. See docs/switch_pro_protocol.md "Connection direction".
esp_timer_handle_t s_kick_timer = nullptr;
uint8_t s_kick_attempts = 0;
constexpr uint8_t  kKickMaxAttempts  = 4;
// Host grace: long enough that hosts which open both channels themselves (Switch console, 8BitDo
// Retro Receiver, both well under 1 s) always win and cancel the kick, but short enough to beat
// BlueRetro, which opens CTRL and waits only ~2 s for the controller's INTR before tearing down
// and retrying - a 3 s grace loses that race and the two sides then collide (orphan L2CAP channel,
// subcommand replies lost, link dropped after ~30-60 s, repeat).
constexpr uint64_t kKickGraceUs      = 1500 * 1000;
constexpr uint64_t kKickRetryUs      = 5 * 1000 * 1000;

void hid_kick_cb(void*) {
    if (s_connected) return;
    static const esp_bd_addr_t kZeroAddr = {0};
    if (memcmp(s_peer, kZeroAddr, sizeof(esp_bd_addr_t)) == 0) return;
    if (s_kick_attempts >= kKickMaxAttempts) {
        ESP_LOGW(TAG, "device-initiated HID: giving up after %u attempts, back to passive",
                 s_kick_attempts);
        return;
    }
    s_kick_attempts++;
    ESP_LOGI(TAG, "host has not opened HID, device-initiating (attempt %u/%u)",
             s_kick_attempts, kKickMaxAttempts);
    esp_err_t err = esp_bt_hid_device_connect(s_peer);
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_bt_hid_device_connect: %s", esp_err_to_name(err));
    esp_timer_start_once(s_kick_timer, kKickRetryUs);     // retry unless CONNECT stops us
}

void enter_discoverable() {
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

void arm_hid_kick(bool reset_attempts) {
    if (!s_kick_timer) return;
    if (reset_attempts) s_kick_attempts = 0;
    esp_timer_stop(s_kick_timer);
    esp_timer_start_once(s_kick_timer, kKickGraceUs);
}

void cancel_hid_kick() {
    if (s_kick_timer) esp_timer_stop(s_kick_timer);
    s_kick_attempts = 0;
}

// --- Profiles: which Pro buttons the NES face buttons produce ----------------------------------
// Select/Start always map to Minus/Plus. Directions are handled by the directional mode, not here.
struct ProProfile {
    const char* name;
    // Pro button each NES button activates, addressed by a small enum so the table stays readable.
    enum Btn { A, B, X, Y } nes_a, nes_b;
};
const ProProfile kProfiles[] = {
    // "Literal": A->A, B->B. Unambiguous in the Switch "Test Input Devices" screen.
    { "Literal", ProProfile::A, ProProfile::B },
    // "NSO NES": A->B, B->Y, the standard NSO NES layout (NES A = jump = Switch B).
    { "NSO NES", ProProfile::B, ProProfile::Y },
};
constexpr uint8_t kNumProfiles = sizeof(kProfiles) / sizeof(kProfiles[0]);

// --- Directional modes: where the NES d-pad goes ----------------------------------------------
enum ProDirMode : uint8_t { DIR_DPAD = 0, DIR_STICK = 1, DIR_BOTH = 2 };
const char* kDirModeNames[] = { "D-Pad", "Left Stick", "Both" };
constexpr uint8_t kNumDirModes = sizeof(kDirModeNames) / sizeof(kDirModeNames[0]);

void apply_profile_button(ProState& s, ProProfile::Btn which, bool pressed) {
    if (!pressed) return;
    switch (which) {
    case ProProfile::A: s.a = true; break;
    case ProProfile::B: s.b = true; break;
    case ProProfile::X: s.x = true; break;
    case ProProfile::Y: s.y = true; break;
    }
}

ProState map_input(const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    if (profile >= kNumProfiles) profile = 0;
    if (dir_mode >= kNumDirModes) dir_mode = DIR_DPAD;
    const ProProfile& p = kProfiles[profile];

    ProState s;
    apply_profile_button(s, p.nes_a, in.a);
    apply_profile_button(s, p.nes_b, in.b);
    s.minus = in.select;
    s.plus  = in.start;

    if (dir_mode == DIR_DPAD || dir_mode == DIR_BOTH) {
        s.up = in.up; s.down = in.down; s.left = in.left; s.right = in.right;
    }
    if (dir_mode == DIR_STICK || dir_mode == DIR_BOTH) {
        s.lx = (in.right ? 1 : 0) - (in.left ? 1 : 0);
        s.ly = (in.up ? 1 : 0) - (in.down ? 1 : 0);   // Pro stick: up is positive
    }
    return s;
}

// --- HID report descriptor (parses cleanly, declares 0x30/0x21/0x01/0x10) ----------------------
const uint8_t kProReportMap[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x06, 0x00, 0xFF, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x30,
    0x85, 0x30, 0x09, 0x30, 0x81, 0x02,
    0x85, 0x21, 0x09, 0x21, 0x81, 0x02,
    0x85, 0x3F, 0x09, 0x3F, 0x95, 0x0B, 0x81, 0x02,
    0x85, 0x01, 0x09, 0x01, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x91, 0x02,
    0xC0
};

esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = kProReportMap, .len = sizeof(kProReportMap) },
};

esp_hid_device_config_t s_hid_config = {
    .vendor_id         = 0x057E,          // Nintendo
    .product_id        = 0x2009,          // Pro Controller
    .version           = 0x0001,
    .device_name       = "Pro Controller",
    .manufacturer_name = "Nintendo",
    .serial_number     = "000000000001",
    .report_maps       = s_report_maps,
    .report_maps_len   = 1,
};

// --- Switch Pro report packing + handshake (see docs/switch_pro_protocol.md) -------------------
uint8_t s_timer = 0;
// A real Pro Controller powers up in simple input mode (report 0x3F, sent on change) and only
// streams full 0x30 reports after the host selects that mode with subcommand 0x03. The Switch
// console and the 8BitDo adapters send 0x03 during their handshake; BlueRetro never does and
// reads 0x3F reports, dropping a controller that stays silent (~20 s watchdog).
volatile uint8_t s_input_mode = 0x3F;

const uint8_t kCal603D[] = {
    0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0xF0,0x07,0x7F,
    0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0xF0,0x07,0x7F, 0x0F,0x0F, 0x00,0x00,0x00,0x00,0x00};
const uint8_t kCal6080[] = {
    0x5e,0x01,0x00,0x00,0xf1,0x0f, 0x19,0xd0,0x4c,0xae,0x40,0xe1,
    0x00,0x00,0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff,0xff,0xff};
const uint8_t kCal6098[] = {
    0x19,0xd0,0x4c,0xae,0x40,0xe1, 0x00,0x00,0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff,0xff,0xff};
const uint8_t kElapsed04[] = {0x00,0x6a,0x01,0xbb,0x01,0x93,0x01,0x95,0x01};

void pack_stick(uint8_t* o, uint16_t x, uint16_t y) {
    o[0] = x & 0xFF;
    o[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    o[2] = (y >> 4) & 0xFF;
}

void pack_buttons(uint8_t b[3], const ProState& s) {
    b[0] = (s.y?0x01:0)|(s.x?0x02:0)|(s.b?0x04:0)|(s.a?0x08:0)|(s.r?0x40:0)|(s.zr?0x80:0);
    b[1] = (s.minus?0x01:0)|(s.plus?0x02:0)|(s.r_stick?0x04:0)|(s.l_stick?0x08:0)|
           (s.home?0x10:0)|(s.capture?0x20:0);
    b[2] = (s.down?0x01:0)|(s.up?0x02:0)|(s.right?0x04:0)|(s.left?0x08:0)|(s.l?0x40:0)|(s.zl?0x80:0);
}

// Left stick: map -1/0/+1 to the 12-bit min/center/max the calibration expects.
void pack_left_stick(uint8_t* o, int16_t lx, int16_t ly) {
    auto axis = [](int16_t v) -> uint16_t {
        if (v > 0) return 3500;
        if (v < 0) return 600;
        return 2048;
    };
    pack_stick(o, axis(lx), axis(ly));
}

void fill_prefix(uint8_t* b, const ProState* st, const uint8_t btn[3]) {
    b[0] = s_timer++;
    b[1] = s_battery;
    b[2] = btn[0]; b[3] = btn[1]; b[4] = btn[2];
    if (st) pack_left_stick(b + 5, st->lx, st->ly);
    else    pack_stick(b + 5, 2048, 2048);
    pack_stick(b + 8, 2048, 2048);           // right stick centered (NES has none)
    b[11] = 0x80;
}

void send_reply(uint8_t ack, uint8_t subcmd, const uint8_t* payload, size_t plen) {
    uint8_t b[48] = {0};
    const uint8_t no_btn[3] = {0, 0, 0};
    fill_prefix(b, nullptr, no_btn);
    b[12] = ack;
    b[13] = subcmd;
    if (payload && plen) {
        if (plen > sizeof(b) - 14) plen = sizeof(b) - 14;
        memcpy(b + 14, payload, plen);
    }
    if (s_dev) esp_hidd_dev_input_set(s_dev, 0, 0x21, b, sizeof(b));
}

void handle_spi_read(const uint8_t* args) {
    uint32_t addr = args[0] | (args[1] << 8) | (args[2] << 16) | ((uint32_t)args[3] << 24);
    uint8_t size = args[4];
    if (size > 0x1D) size = 0x1D;
    uint8_t payload[5 + 0x1D] = {0};
    memcpy(payload, args, 5);
    const uint8_t* blob = nullptr; size_t blen = 0;
    switch (addr) {
    case 0x603D: blob = kCal603D; blen = sizeof(kCal603D); break;
    case 0x6080: blob = kCal6080; blen = sizeof(kCal6080); break;
    case 0x6098: blob = kCal6098; blen = sizeof(kCal6098); break;
    case 0x8010: case 0x8028: memset(payload + 5, 0xFF, size); break;
    default: break;
    }
    if (blob) memcpy(payload + 5, blob, blen < size ? blen : size);
    send_reply(0x90, 0x10, payload, 5 + size);
}

void handle_device_info() {
    const uint8_t* mac = esp_bt_dev_get_address();
    uint8_t p[12] = {0x03, 0x48, 0x03, 0x02, 0,0,0,0,0,0, 0x01, 0x02};
    if (mac) memcpy(p + 4, mac, 6);
    send_reply(0x82, 0x02, p, sizeof(p));
}

void handle_subcommand(const uint8_t* data, size_t len) {
    if (len < 11) return;
    uint8_t subcmd = data[9];
    const uint8_t* args = data + 10;
    ESP_LOGI(TAG, "subcommand 0x%02x", subcmd);
    switch (subcmd) {
    case 0x02: handle_device_info(); break;
    case 0x10: handle_spi_read(args); break;
    case 0x04: send_reply(0x83, 0x04, kElapsed04, sizeof(kElapsed04)); break;
    case 0x01: send_reply(0x80, 0x01, nullptr, 0); break;
    case 0x03:
        // Set input report mode: 0x30 (or 0x31..) = full-report streaming, 0x3F = simple mode.
        s_input_mode = (args[0] == 0x3F) ? 0x3F : 0x30;
        ESP_LOGI(TAG, "input mode -> 0x%02x", s_input_mode);
        send_reply(0x80, 0x03, nullptr, 0);
        break;
    case 0x08: send_reply(0x80, 0x08, nullptr, 0); break;
    case 0x21: send_reply(0x80, 0x21, nullptr, 0); break;
    case 0x30: send_reply(0x80, 0x30, nullptr, 0); break;
    case 0x40: send_reply(0x80, 0x40, nullptr, 0); break;
    case 0x48: send_reply(0x80, 0x48, nullptr, 0); break;
    default:   send_reply(0x80, subcmd, nullptr, 0); break;
    }
}

// Simple-mode (0x3F) input report: 2 button bytes, hat, then 4 x 16-bit axes (0x8000 = center).
void send_3f_report(const ProState& s) {
    uint8_t b[11] = {0};
    b[0] = (s.b?0x01:0)|(s.a?0x02:0)|(s.y?0x04:0)|(s.x?0x08:0)|(s.l?0x10:0)|(s.r?0x20:0)|
           (s.zl?0x40:0)|(s.zr?0x80:0);
    b[1] = (s.minus?0x01:0)|(s.plus?0x02:0)|(s.l_stick?0x04:0)|(s.r_stick?0x08:0)|
           (s.home?0x10:0)|(s.capture?0x20:0);
    static const uint8_t hat_lut[3][3] = {   // [down..up][left..right] -> hat code, 8 = neutral
        {5, 4, 3},    // down:  down-left, down, down-right
        {6, 8, 2},    // mid:   left, neutral, right
        {7, 0, 1},    // up:    up-left, up, up-right
    };
    int v = 1 + (s.up ? 1 : 0) - (s.down ? 1 : 0);
    int h = 1 + (s.right ? 1 : 0) - (s.left ? 1 : 0);
    b[2] = hat_lut[v][h];
    auto axis16 = [](int16_t a) -> uint16_t { return a > 0 ? 0xFFFF : a < 0 ? 0x0000 : 0x8000; };
    uint16_t lx = axis16(s.lx), ly16 = axis16((int16_t)-s.ly);   // 0x3F Y axis: down is positive
    b[3] = lx & 0xFF; b[4] = lx >> 8;
    b[5] = ly16 & 0xFF; b[6] = ly16 >> 8;
    b[7] = 0x00; b[8] = 0x80; b[9] = 0x00; b[10] = 0x80;         // right stick centered
    esp_hidd_dev_input_set(s_dev, 0, 0x3F, b, sizeof(b));
}

void stream_task(void*) {
    uint8_t b[48];
    bool streaming = false;
    while (true) {
        if (s_connected && s_dev) {
            if (!streaming) {
                vTaskDelay(pdMS_TO_TICKS(1500));   // let connection setup + sniff negotiation settle
                if (!s_connected) continue;
                streaming = true;
                ESP_LOGI(TAG, "link settled -> streaming (mode 0x%02x)", s_input_mode);
            }
            ProState snapshot = s_state;           // copy so packing is consistent
            uint8_t btn[3];
            pack_buttons(btn, snapshot);

            if (s_input_mode == 0x30) {
                // Full mode: continuous ~66 Hz 0x30 stream (the Switch drops a quiet controller).
                memset(b, 0, sizeof(b));
                fill_prefix(b, &snapshot, btn);
                esp_hidd_dev_input_set(s_dev, 0, 0x30, b, sizeof(b));
            } else {
                // Simple mode (real-Pro default; BlueRetro stays here): 0x3F on change, plus a
                // ~100 ms keepalive so a quiet controller is not reaped by host watchdogs.
                static uint8_t p3f0 = 0xFF, p3f1 = 0xFF, p3f2 = 0xFF;
                static TickType_t last_3f = 0;
                uint8_t hb0 = btn[0], hb1 = btn[1], hb2 = btn[2];
                bool changed = (hb0 != p3f0 || hb1 != p3f1 || hb2 != p3f2);
                if (changed || (xTaskGetTickCount() - last_3f) >= pdMS_TO_TICKS(100)) {
                    send_3f_report(snapshot);
                    p3f0 = hb0; p3f1 = hb1; p3f2 = hb2;
                    last_3f = xTaskGetTickCount();
                }
            }

            static uint8_t pb0 = 0, pb1 = 0, pb2 = 0;
            static TickType_t last_tx = 0;
            if ((btn[0] != pb0 || btn[1] != pb1 || btn[2] != pb2) &&
                (xTaskGetTickCount() - last_tx) >= pdMS_TO_TICKS(50)) {
                ESP_LOGI(TAG, "TX 0x%02x btn=%02x %02x %02x", s_input_mode, btn[0], btn[1], btn[2]);
                pb0 = btn[0]; pb1 = btn[1]; pb2 = btn[2];
                last_tx = xTaskGetTickCount();
            }
        } else {
            streaming = false;
        }
        // Wait up to 15 ms (>=66 Hz keepalive the Switch handshake expects), but return immediately
        // when pro_set_input() notifies us of an input change, so a press is not sitting idle for up
        // to a full stream period before it ships. Coalesces multiple notifies (pdTRUE clears count).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15));
    }
}

// --- BT Classic GAP -----------------------------------------------------------------------------
void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "GAP auth OK: %s, waiting for host to open HID connection",
                     param->auth_cmpl.device_name);
            memcpy(s_peer, param->auth_cmpl.bda, sizeof(esp_bd_addr_t));
            arm_hid_kick(true);   // if the host stays quiet (8BitDo Adapter 2 style), we initiate
        } else {
            ESP_LOGE(TAG, "GAP auth FAILED: stat=%d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        // The actually-connected host, captured before HID CONNECT so the QoS request there pins the
        // right link. s_peer is otherwise seeded from bonds[0] on a reconnect (see HIDD START); with
        // >=2 stored bonds, a resume from a host that is NOT bonds[0] and skips a fresh GAP auth (a
        // stored link key resumes without re-auth) would leave s_peer stale and QoS the wrong address.
        // ACL_CONN_CMPL fires for both fresh and resumed links and carries the real
        // peer, so it is the authoritative source; the bond seed remains only the device-kick target.
        if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
            memcpy(s_peer, param->acl_conn_cmpl_stat.bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "ACL up: %02x:%02x:%02x:%02x:%02x:%02x -> QoS target",
                     s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
        }
        break;
    case ESP_BT_GAP_QOS_CMPL_EVT:
        ESP_LOGI(TAG, "GAP QoS set (stat=%d, t_poll=%" PRIu32 ") -> link pinned active",
                 param->qos_cmpl.stat, param->qos_cmpl.t_poll);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "GAP MODE_CHG mode=%d", param->mode_chg.mode);
        break;
    default:
        break;
    }
}

#if defined(CONFIG_BT_SDP_COMMON_ENABLED)
void sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t* param) {
    if (event == ESP_SDP_INIT_EVT && param->init.status == ESP_SDP_SUCCESS) {
        esp_bluetooth_sdp_dip_record_t dip = {};
        dip.hdr.type         = ESP_SDP_TYPE_DIP_SERVER;
        dip.vendor           = s_hid_config.vendor_id;
        dip.vendor_id_source = ESP_SDP_VENDOR_ID_SRC_USB;
        dip.product          = s_hid_config.product_id;
        dip.version          = s_hid_config.version;
        dip.primary_record   = true;
        esp_sdp_create_record(reinterpret_cast<esp_bluetooth_sdp_record_t*>(&dip));
    }
}
#endif

void hidd_cb(void*, esp_event_base_t, int32_t id, void* event_data) {
    auto event = static_cast<esp_hidd_event_t>(id);
    auto* p = static_cast<esp_hidd_event_data_t*>(event_data);

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HIDD START (status=%d) -> connectable + discoverable", p->start.status);
        enter_discoverable();
        s_link = bt::LINK_ADVERTISING;
        // Start passive for both fresh pair and reconnect: hosts that initiate HID themselves (the
        // Switch "Change Grip/Order" screen, the 8BitDo Retro Receiver) get the grace window and
        // connect normally. Hosts that mimic the console's reconnect role and wait for the
        // controller to open the channels (8BitDo USB Adapter 2, BlueRetro, a console after a power
        // cycle) get the bounded device-initiated kick (arm_hid_kick). Device-initiating puts
        // Bluedroid's HID-device state machine into an initiator state that can strand a host's own
        // simultaneous incoming HID connection, which is why the kick only fires after the host has
        // stayed quiet for the whole grace window. See docs/switch_pro_protocol.md
        // "Connection direction".
        if (s_have_bond) {
            // On a reconnect there may be no fresh GAP auth before CONNECT, so seed the peer from the
            // bond now, otherwise the QoS request on CONNECT targets a zero address and fails (0x7).
            // The seeded peer also gives the boot-time kick its target: a host that never pages us
            // (8BitDo Adapter 2) is re-joined by us paging it.
            memcpy(s_peer, s_bonded, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "reconnect: discoverable, host may initiate or we kick after grace");
            arm_hid_kick(true);
        } else {
            ESP_LOGI(TAG, "fresh pair: discoverable, waiting for a host");
        }
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HIDD CONNECT (status=%d, %s)", p->connect.status,
                 s_have_bond ? "resume" : "fresh");
        if (p->connect.status == ESP_OK) {
            s_input_mode = 0x3F;   // every fresh connection starts in simple mode, like a real Pro
            s_connected = true;
            s_link = bt::LINK_CONNECTED;
            cancel_hid_kick();
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            esp_bt_gap_set_qos(s_peer, kQosTpoll);
        } else {
            // A failed/torn-down connect leaves the ACL down; re-assert discoverable so the host can
            // page us again for another try (the kick's own retry timer keeps running if armed).
            s_link = bt::LINK_ADVERTISING;
            enter_discoverable();
        }
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        if (p->output.report_id == 0x01) {
            handle_subcommand(p->output.data, p->output.length);
        } else if (p->output.report_id != 0x10) {
            ESP_LOGW(TAG, "unexpected OUTPUT id=0x%02x len=%d", p->output.report_id, p->output.length);
        }
        break;
    case ESP_HIDD_FEATURE_EVENT:
        ESP_LOGI(TAG, "HIDD FEATURE id=0x%02x len=%d", p->feature.report_id, p->feature.length);
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "HIDD DISCONNECT (reason=%d) -> re-advertise", p->disconnect.reason);
        s_connected = false;
        s_link = bt::LINK_ADVERTISING;
        enter_discoverable();
        arm_hid_kick(true);   // a power-cycled host waits for us to re-join
        break;
    case ESP_HIDD_STOP_EVENT:
        ESP_LOGI(TAG, "HIDD STOP");
        s_link = bt::LINK_IDLE;
        break;
    default:
        break;
    }
}

// --- Ops implementation -------------------------------------------------------------------------
void pro_init() {
    ESP_LOGI(TAG, "Switch Pro / BT Classic, bringing up Bluedroid (BR/EDR only this boot)");

    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // one radio at a time: free the BLE half

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32)
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;
#endif
    bt_cfg.bt_max_acl_conn = 3;
    bt_cfg.bt_max_sync_conn = 3;
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

#if defined(CONFIG_BTDM_CTRL_MODEM_SLEEP)
    // Modem sleep is compiled in for the BLE transport's power savings (see sdkconfig.defaults),
    // but Classic runs with it disabled: it must not touch the fragile Switch handshake, and it
    // saves nothing on the connected link anyway - the QoS-pinned active ACL (kQosTpoll below)
    // keeps RX on regardless.
    ESP_ERROR_CHECK(esp_bt_sleep_disable());
#endif

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    // Secure Connections host support: the Switch 2 reads our LMP feature page 1 right after the
    // ACL comes up and refuses to even start SSP unless the SC host-support bit is set (it drops
    // the link with reason 0x05 ~60 ms in). The ESP32 controller supports SC (feature page 2);
    // stock IDF only vetoes the host bit as an AES-CCM-workaround precaution, overridden by
    // tools/patch_bluedroid_sc_mode.py. The Switch 1 and 8BitDo hosts ignore the bit.
    bd_cfg.sc_en = true;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;     // "just works" pairing (no PIN)
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_cb));

    // Connection state machine: a stored bond means this boot is a reconnect (resume the same
    // host); none means a fresh pair. Forget rotates our MAC, so after a forget there is no bond
    // here and the (now differently-addressed) device pairs cleanly instead of half-reconnecting.
    int nbonds = esp_bt_gap_get_bond_device_num();
    ESP_LOGI(TAG, "stored BT bonds at boot: %d", nbonds);
    if (nbonds > 0) {
        esp_bd_addr_t bonds[8];
        int n = nbonds > 8 ? 8 : nbonds;
        if (esp_bt_gap_get_bond_device_list(&n, bonds) == ESP_OK && n > 0) {
            memcpy(s_bonded, bonds[0], sizeof(esp_bd_addr_t));
            s_have_bond = true;
        }
    }

    esp_bt_gap_set_device_name(s_hid_config.device_name);

    // Full real-Pro Class of Device 0x002508, including the "Limited Discoverable" service-class
    // bit. The Switch 2's Grip-menu inquiry pages a controller only if its CoD matches a real
    // Pro's byte-for-byte (bench A/B: a BlueZ host presenting 0x002508 with a generic MAC and a
    // junk-filled EIR was paged within 2 s of the console entering the screen; this device with
    // 0x000508 and a byte-clean EIR was never paged. The Switch 1 accepts either, and neither
    // console queries SDP before pairing, so the CoD is the whole gate).
    // This must be set BEFORE HID startup enables inquiry scan: the ESP32 controller latches the
    // CoD into its inquiry response at scan-enable and ignores later writes. Stock Bluedroid would
    // strip the service bit again during the scan-mode change; patch_bluedroid_cod.py makes that
    // strip additive-only so this CoD survives. Verify at the radio: `hcitool inq` -> 0x002508.
    esp_bt_cod_t cod = {};
    cod.major   = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor   = ESP_BT_COD_MINOR_PERIPHERAL_GAMEPAD;
    cod.service = ESP_BT_COD_SRVC_LMTD_DISCOVER;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

    const esp_timer_create_args_t kick_args = {
        .callback = hid_kick_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "hid_kick",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&kick_args, &s_kick_timer));

    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BT, hidd_cb, &s_dev));

#if defined(CONFIG_BT_SDP_COMMON_ENABLED)
    ESP_ERROR_CHECK(esp_sdp_register_callback(sdp_cb));
    ESP_ERROR_CHECK(esp_sdp_init());
#endif

    xTaskCreate(stream_task, "pro_stream", 4096, nullptr, 5, &s_stream_task);
    ESP_LOGI(TAG, "init done, pair from the Switch 'Change Grip/Order' screen or an 8BitDo receiver");
}

bt::LinkState pro_link_state() { return s_link; }

void pro_set_input(const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    s_state = map_input(in, profile, dir_mode);
    // app_main only calls set_input() on an actual button-state change, so wake the stream task now
    // to ship this snapshot immediately instead of waiting for its next periodic 15 ms tick.
    if (s_stream_task) xTaskNotifyGive(s_stream_task);
}

void pro_clear_input() { s_state = ProState{}; }

uint8_t     pro_num_profiles()          { return kNumProfiles; }
const char* pro_profile_name(uint8_t i) { return i < kNumProfiles ? kProfiles[i].name : "?"; }
uint8_t     pro_num_dir_modes()         { return kNumDirModes; }
const char* pro_dir_mode_name(uint8_t i){ return i < kNumDirModes ? kDirModeNames[i] : "?"; }

void pro_set_battery_level(uint8_t pct) {
    // Pro input reports carry a 4-bit battery nibble in the high bits of byte 1 (8=full .. 0=empty).
    uint8_t nib = pct >= 70 ? 0x8 : pct >= 40 ? 0x6 : pct >= 20 ? 0x4 : pct >= 10 ? 0x2 : 0x0;
    s_battery = nib << 4;
}

void pro_forget_host() {
    // Forget: drop every bond and rotate our BT identity, then reboot. The reboot brings us up with a
    // new MAC and no bond, so the previously-paired host no longer recognises us; it cannot
    // auto-reconnect to a stale address and skip the handshake.
    ESP_LOGW(TAG, "forget host -> remove bonds + rotate identity + reboot");
    cancel_hid_kick();
    if (s_connected) esp_bt_hid_device_virtual_cable_unplug();

    bt::clear_all_bonds();   // both BR/EDR and BLE tables: the identity bump invalidates all
    settings::bump_identity_generation();
    vTaskDelay(pdMS_TO_TICKS(200));   // let NVS commit + the unplug flush
    esp_restart();
}

const bt::Ops s_ops = {
    .name                  = "Classic (Switch Pro)",
    .init                  = pro_init,
    .link_state            = pro_link_state,
    .set_input             = pro_set_input,
    .clear_input           = pro_clear_input,
    .num_profiles          = pro_num_profiles,
    .profile_name          = pro_profile_name,
    .num_directional_modes = pro_num_dir_modes,
    .directional_mode_name = pro_dir_mode_name,
    .forget_host           = pro_forget_host,
    .set_battery_level     = pro_set_battery_level,
};

} // namespace

const bt::Ops* bt_pro_ops() { return &s_ops; }