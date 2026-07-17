// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// BLE transport: a "NES Advantage" BLE GATT HID gamepad for modern hosts (phones, PCs, Windows).
// A 5-byte report (12 buttons + hat + X/Y) on Bluedroid's unified esp_hidd. One radio at a time:
// this boot brings up BLE only and frees the Classic half. Identity is "NES Advantage" (not the Pro
// Controller).
//
// Two gamepads, one connection: the gamepad report map is registered twice, so esp_hidd stands up
// two BLE HID GATT services (index 0 = Player 1, index 1 = Player 2). A PC or emulator enumerates two
// separate gamepads, and the Advantage's player-select slider hands the live stick between them, the
// original take-turns behaviour. One report map with two top-level collections does not work on Linux
// (both collections share the "Gamepad" usage, so hid-input merges them into a single js device).
// Console receivers bind one BT connection to one player and use the first service; on BlueRetro,
// double-map that controller to both wired ports for hot-seat play.
//
// Profiles and directional modes: button-number remaps plus d-pad/axes/both routing.

#include "bt_transport.hpp"
#include "settings.hpp"

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_common.h"

static const char* TAG = "bt_ble";

namespace {

constexpr uint8_t kPlayers = 2;    // two HID services (P1/P2); the player-select slider picks live

esp_hidd_dev_t* s_dev = nullptr;
volatile bt::LinkState s_link = bt::LINK_IDLE;
uint8_t s_report[kPlayers][5] = {{0}};  // live HID report per player (2 button bytes, hat, X, Y)
uint8_t s_battery = 100;

// --- Report descriptors: one NES gamepad each (12 buttons + hat + X/Y), parameterised by HID Report
// ID. We build it twice with DISTINCT ids (1 and 2). /* */ comments + trailing '\' keep the macro one
// logical line; do NOT use // here (it would swallow the continuation).
#define NES_GAMEPAD_DESC(REPORT_ID)                                                              \
    0x05, 0x01,        /* Usage Page (Generic Desktop) */                                        \
    0x09, 0x05,        /* Usage (Gamepad) */                                                     \
    0xA1, 0x01,        /* Collection (Application) */                                            \
    0x85, (REPORT_ID), /*   Report ID */                                                         \
    0x05, 0x09,        /*   Usage Page (Button) */                                               \
    0x19, 0x01,        /*   Usage Minimum (Button 1) */                                          \
    0x29, 0x0C,        /*   Usage Maximum (Button 12) */                                         \
    0x15, 0x00,        /*   Logical Minimum (0) */                                               \
    0x25, 0x01,        /*   Logical Maximum (1) */                                               \
    0x75, 0x01,        /*   Report Size (1) */                                                   \
    0x95, 0x0C,        /*   Report Count (12) */                                                 \
    0x81, 0x02,        /*   Input (Data,Var,Abs) */                                              \
    0x75, 0x01,        /*   Report Size (1)   4-bit pad to a byte boundary */                    \
    0x95, 0x04,        /*   Report Count (4) */                                                  \
    0x81, 0x03,        /*   Input (Const,Var,Abs) */                                             \
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */                                      \
    0x09, 0x39,        /*   Usage (Hat Switch) */                                                \
    0x15, 0x01,        /*   Logical Minimum (1) */                                               \
    0x25, 0x08,        /*   Logical Maximum (8) */                                               \
    0x35, 0x00,        /*   Physical Minimum (0) */                                              \
    0x46, 0x3B, 0x01,  /*   Physical Maximum (315) */                                            \
    0x65, 0x14,        /*   Unit (Degrees) */                                                    \
    0x75, 0x04,        /*   Report Size (4) */                                                   \
    0x95, 0x01,        /*   Report Count (1) */                                                  \
    0x81, 0x02,        /*   Input (Data,Var,Abs) */                                              \
    0x75, 0x01,        /*   Report Size (1)   4-bit pad */                                       \
    0x95, 0x04,        /*   Report Count (4) */                                                  \
    0x81, 0x03,        /*   Input (Const,Var,Abs) */                                             \
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */                                      \
    0x09, 0x01,        /*   Usage (Pointer) */                                                   \
    0xA1, 0x00,        /*   Collection (Physical) */                                             \
    0x09, 0x30,        /*     Usage (X) */                                                       \
    0x09, 0x31,        /*     Usage (Y) */                                                       \
    0x15, 0x81,        /*     Logical Minimum (-127) */                                          \
    0x25, 0x7F,        /*     Logical Maximum (127) */                                           \
    0x75, 0x08,        /*     Report Size (8) */                                                 \
    0x95, 0x02,        /*     Report Count (2) */                                                \
    0x81, 0x02,        /*     Input (Data,Var,Abs) */                                            \
    0xC0,              /*   End Collection */                                                    \
    0xC0               /* End Collection */

const uint8_t kReportMapId1[] = { NES_GAMEPAD_DESC(0x01) };   // service index 0, Report ID 1
const uint8_t kReportMapId2[] = { NES_GAMEPAD_DESC(0x02) };   // service index 1, Report ID 2
#undef NES_GAMEPAD_DESC

// Two HID GATT service instances on one connection. esp_hidd's BLE backend registers one HID service
// per report map (ble_hidd.c: app_register(HID_SVC + i)), so a Linux or Windows host enumerates two
// separate gamepads. Structure:
//   - Two services, not one map with two collections: on Linux both collections share the "Gamepad"
//     usage, so hid-input merges them into a single js device.
//   - Distinct report ids (1 vs 2): esp_hidd's BLE send resolves the report by (id,type) across all
//     services and returns the first match (ble_hidd.c: get_report_by_id_and_type). Identical ids
//     would both resolve to service 0, so every player's input lands on one js node.
esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = kReportMapId1, .len = sizeof(kReportMapId1) },   // service index 0
    { .data = kReportMapId2, .len = sizeof(kReportMapId2) },   // service index 1
};

// Player to service routing. The kernel numbers the LAST-registered HID service as the LOWER js node
// (observed: service index 1 -> js0, index 0 -> js1), so route P1 to index 1 (js0) and P2 to index 0
// (js1) to match player numbers to node numbers. report_id MUST match the chosen service's report map
// (index 0 = id 1, index 1 = id 2) so the by-id lookup above resolves to that service. If a host ever
// numbers them the other way, swap these two rows.
struct BleRoute { uint8_t map_index; uint8_t report_id; };
constexpr BleRoute kRoute[kPlayers] = {
    /* P1 */ { 1, 2 },   // service index 1 -> js0
    /* P2 */ { 0, 1 },   // service index 0 -> js1
};

esp_hid_device_config_t s_hid_config = {
    .vendor_id         = 0x42CA,          // PnP IDs (kept for host-mapping continuity)
    .product_id        = 0x42CD,
    .version           = 0x0110,
    .device_name       = "NES Advantage",
    .manufacturer_name = "Cajun Panda's Retro Gaming",
    .serial_number     = "000000000001",
    .report_maps       = s_report_maps,
    .report_maps_len   = kPlayers,    // two HID services: index 0 = P1, index 1 = P2
};

// --- BLE advertising: HID appearance + HID service UUID, just-works bonding --------------------
constexpr uint16_t kAppearanceGamepad = 0x03C4;

uint8_t s_service_uuid[16] = {   // 128-bit form of the 16-bit HID service UUID 0x1812
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

// Advertising packet (max 31 bytes): flags + appearance + 128-bit HID service UUID + tx power.
// The name does not also fit here, so it goes in the SCAN RESPONSE instead.
esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = kAppearanceGamepad,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = sizeof(s_service_uuid),
    .p_service_uuid      = s_service_uuid,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// Scan response: just the device name ("NES Advantage").
esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = false,
    .min_interval        = 0,
    .max_interval        = 0,
    .appearance          = 0,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = 0,
    .p_service_uuid      = nullptr,
    .flag                = 0,
};

esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x30,
    .adv_type          = ADV_TYPE_IND,
    // Advertise from a static random address (set in ble_init), not the chip's public BT MAC, so a
    // dual-mode host that paired us over Classic first still sees the BLE gamepad as a separate device
    // and enumerates its LE HID.
    .own_addr_type     = BLE_ADDR_TYPE_RANDOM,
    .peer_addr         = {0},
    .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// --- Profiles: NES face buttons -> HID button numbers (1..12) ----------------------------------
struct BleProfile { const char* name; uint8_t a, b, select, start; };
const BleProfile kProfiles[] = {
    { "Default",    1, 2, 11, 12 },
    { "Blue Retro", 1, 4, 11, 12 },
};
constexpr uint8_t kNumProfiles = sizeof(kProfiles) / sizeof(kProfiles[0]);

// The chord layer's virtual buttons (see bt::NesInput). No profile applies, so they take fixed
// numbers from the range no profile uses (3 and 5..10 are free of the 12 declared). 5..8 keeps them
// contiguous and lands ZL/ZR where a PC or emulator's default map expects shoulders.
constexpr uint8_t kBtnZL = 5, kBtnZR = 6, kBtnHome = 7, kBtnCapture = 8;

enum BleDirMode : uint8_t { DIR_DPAD = 0, DIR_AXES = 1, DIR_BOTH = 2 };
const char* kDirModeNames[] = { "D-Pad", "Axes", "Both" };
constexpr uint8_t kNumDirModes = sizeof(kDirModeNames) / sizeof(kDirModeNames[0]);

void set_button(uint8_t* rep, uint8_t btn /*1..12*/, bool pressed) {
    if (btn < 1 || btn > 12 || !pressed) return;
    uint8_t bit = btn - 1;
    rep[bit / 8] |= (1 << (bit % 8));
}

uint8_t hat_from_dirs(const bt::NesInput& in) {
    if (in.up && in.right)   return 2;
    if (in.right && in.down) return 4;
    if (in.down && in.left)  return 6;
    if (in.left && in.up)    return 8;
    if (in.up)    return 1;
    if (in.right) return 3;
    if (in.down)  return 5;
    if (in.left)  return 7;
    return 0;
}

void build_report(uint8_t* rep, const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    if (profile >= kNumProfiles) profile = 0;
    if (dir_mode >= kNumDirModes) dir_mode = DIR_DPAD;
    const BleProfile& p = kProfiles[profile];
    memset(rep, 0, 5);

    set_button(rep, p.a, in.a);
    set_button(rep, p.b, in.b);
    set_button(rep, p.select, in.select);
    set_button(rep, p.start, in.start);

    set_button(rep, kBtnZL, in.zl);
    set_button(rep, kBtnZR, in.zr);
    set_button(rep, kBtnHome, in.home);
    set_button(rep, kBtnCapture, in.capture);

    if (dir_mode == DIR_DPAD || dir_mode == DIR_BOTH) rep[2] = hat_from_dirs(in) & 0x0F;
    if (dir_mode == DIR_AXES || dir_mode == DIR_BOTH) {
        rep[3] = (uint8_t)(int8_t)(in.right ? 127 : in.left ? -127 : 0);   // X
        rep[4] = (uint8_t)(int8_t)(in.down  ? 127 : in.up   ? -127 : 0);   // Y (down = +)
    }
}

void notify_report(uint8_t player) {
    if (player >= kPlayers) player = 0;
    if (s_dev && s_link == bt::LINK_CONNECTED) {
        const BleRoute& r = kRoute[player];   // player -> (service index, report id); see kRoute
        esp_hidd_dev_input_set(s_dev, r.map_index, r.report_id, s_report[player], sizeof(s_report[player]));
        esp_hidd_dev_battery_set(s_dev, s_battery);
    }
}

// --- GAP (BLE) ---------------------------------------------------------------------------------
void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_config_adv_data(&s_scan_rsp_data);   // name goes in the scan response
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "advertising as \"NES Advantage\"");
            s_link = bt::LINK_ADVERTISING;
        } else {
            ESP_LOGE(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);   // just works
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        bool ok = param->ble_security.auth_cmpl.success;
        ESP_LOGI(TAG, "BLE auth %s", ok ? "OK" : "FAILED");
        if (ok) {
            // Actively ask the host for the fastest practical connection interval. The adv data only
            // carries a hint (Peripheral Preferred Connection Parameters), which many centrals
            // (Windows especially) ignore, settling on 30 to 50 ms of pure added input lag. 0x06 is
            // 7.5 ms, the BLE 4.2 floor (this ESP32 is 4.2, no 2M PHY); request a pinned 7.5 ms
            // (min==max). Requesting a [7.5, 15] range lets BlueZ settle on 15 ms (it picks the max),
            // so pin the floor instead. The host still has the final say; the granted value is logged
            // on ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT below.
            //
            // Peripheral (slave) latency 4 is the battery-life half of the request: it lets the
            // radio skip listening on up to 4 idle connection events, which with modem sleep powers
            // the radio down between events (BLE connected draw ~114 to ~61 mA at 240 MHz, ~94 to
            // ~41 mA at 80 MHz). Press latency is unchanged: a peripheral with pending
            // data still transmits at the next 7.5 ms event - latency only applies to idle listens.
            // Only host->device traffic (unused in gameplay) can be delayed. A host that refuses
            // simply grants latency 0 and we run at the old draw.
            esp_ble_conn_update_params_t cp = {};
            memcpy(cp.bda, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            cp.min_int = 0x06;     // 6 * 1.25 ms = 7.5 ms
            cp.max_int = 0x06;     // 6 * 1.25 ms = 7.5 ms (pin the floor)
            cp.latency = 4;        // skip up to 4 idle events (radio duty ~1/5 when idle)
            cp.timeout = 400;      // 400 * 10 ms = 4 s supervision timeout
            esp_err_t e = esp_ble_gap_update_conn_params(&cp);
            ESP_LOGI(TAG, "requested fast conn params (7.5 ms pinned, latency 4): %s", esp_err_to_name(e));
        }
        break;
    }
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
        // The actual negotiated link timing, the dominant BLE input-latency term. (interval in
        // 1.25 ms units; an interval of N means a report waits up to N*1.25 ms for its TX slot.)
        auto& u = param->update_conn_params;
        ESP_LOGI(TAG,
                 "conn params updated: interval=%u (=%u.%02u ms) latency=%u timeout=%u ms status=%d",
                 u.conn_int, (unsigned)(u.conn_int * 125 / 100), (unsigned)((u.conn_int * 125) % 100),
                 u.latency, u.timeout * 10, u.status);
        break;
    }
    default:
        break;
    }
}

// --- esp_hidd device events --------------------------------------------------------------------
void hidd_cb(void*, esp_event_base_t, int32_t id, void* event_data) {
    auto event = static_cast<esp_hidd_event_t>(id);
    auto* p = static_cast<esp_hidd_event_data_t*>(event_data);

    switch (event) {
    case ESP_HIDD_START_EVENT:
        // NB: the BLE backend posts START with NULL event data, do not dereference p here.
        ESP_LOGI(TAG, "HIDD START -> configure adv");
        esp_ble_gap_config_adv_data(&s_adv_data);
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HIDD CONNECT (status=%d)", p->connect.status);
        if (p->connect.status == ESP_OK) s_link = bt::LINK_CONNECTED;
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "HIDD PROTOCOL_MODE = %s",
                 p->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "HIDD DISCONNECT (reason=%d) -> re-advertise", p->disconnect.reason);
        s_link = bt::LINK_ADVERTISING;
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_HIDD_STOP_EVENT:
        s_link = bt::LINK_IDLE;
        break;
    default:
        break;
    }
}

// --- Ops implementation ------------------------------------------------------------------------
void ble_init() {
    ESP_LOGI(TAG, "NES Advantage / BLE GATT HID, bringing up Bluedroid (BLE only this boot)");

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);   // one radio at a time: free Classic

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32)
    bt_cfg.mode = ESP_BT_MODE_BLE;
#endif
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_cb));

    // Give BLE its own static-random address (distinct from the Classic public MAC) before any
    // advertising starts, so a dual-mode host that paired us over Classic first still enumerates the
    // LE HID instead of folding it into the classic device record. own_addr_type in
    // s_adv_params is BLE_ADDR_TYPE_RANDOM to match. Set-random-address is async (completes on
    // ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT); advertising only starts after the long HIDD-START ->
    // adv-data -> scan-rsp chain, so the address is in place well before the first ADV.
    esp_bd_addr_t rand_addr;
    settings::ble_static_rand_addr(rand_addr);
    esp_err_t rerr = esp_ble_gap_set_rand_addr(rand_addr);
    ESP_LOGI(TAG, "BLE static random addr %02x:%02x:%02x:%02x:%02x:%02x: %s",
             rand_addr[0], rand_addr[1], rand_addr[2], rand_addr[3], rand_addr[4], rand_addr[5],
             esp_err_to_name(rerr));
    // esp_hidd's BLE backend builds its GATT services from GATTS events, but it does NOT register
    // the GATTS callback itself; the app must route them to esp_hidd_gatts_event_handler, or the
    // HID service is never created and ESP_HIDD_START_EVENT never fires.
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler));
    esp_ble_gap_set_device_name(s_hid_config.device_name);

    // Security: bonding, "just works" (no IO), distribute enc + id keys both ways.
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t   iocap    = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev));
    ESP_LOGI(TAG, "init done, pair from the host's Bluetooth menu (\"NES Advantage\")");
}

bt::LinkState ble_link_state() { return s_link; }

void ble_set_input(const bt::NesInput& in, uint8_t profile, uint8_t dir_mode) {
    uint8_t player = in.player >= kPlayers ? 0 : in.player;
    build_report(s_report[player], in, profile, dir_mode);
    notify_report(player);
}

void ble_clear_input() {
    // Zero BOTH gamepads and push both, so on a player switch the just-vacated joystick returns to
    // neutral on the host (app_main calls this from on_player_change before the new player reports).
    memset(s_report, 0, sizeof(s_report));
    for (uint8_t p = 0; p < kPlayers; p++) notify_report(p);
}

uint8_t     ble_num_profiles()          { return kNumProfiles; }
const char* ble_profile_name(uint8_t i) { return i < kNumProfiles ? kProfiles[i].name : "?"; }
uint8_t     ble_num_dir_modes()         { return kNumDirModes; }
const char* ble_dir_mode_name(uint8_t i){ return i < kNumDirModes ? kDirModeNames[i] : "?"; }

void ble_set_battery_level(uint8_t pct) { s_battery = pct > 100 ? 100 : pct; }

void ble_forget_host() {
    // Same forget semantics as Classic: clear every bond + rotate identity + reboot, so a forgotten
    // host stops auto-reconnecting to the old address.
    ESP_LOGW(TAG, "forget host -> remove bonds + rotate identity + reboot");
    bt::clear_all_bonds();   // both BLE and BR/EDR tables: the identity bump invalidates all
    settings::bump_identity_generation();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

const bt::Ops s_ops = {
    .name                  = "BLE (NES Advantage)",
    .init                  = ble_init,
    .link_state            = ble_link_state,
    .set_input             = ble_set_input,
    .clear_input           = ble_clear_input,
    .num_profiles          = ble_num_profiles,
    .profile_name          = ble_profile_name,
    .num_directional_modes = ble_num_dir_modes,
    .directional_mode_name = ble_dir_mode_name,
    .forget_host           = ble_forget_host,
    .set_battery_level     = ble_set_battery_level,
};

} // namespace

const bt::Ops* bt_ble_ops() { return &s_ops; }