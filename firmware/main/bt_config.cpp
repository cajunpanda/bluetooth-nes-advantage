// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

// BLE configuration + OTA service. Brings up Bluedroid in BLE-only mode and serves a small custom
// GATT service so a browser (Web Bluetooth, see ../../web/) can read/change settings and flash
// new firmware over the air. One radio at a time: Classic is released; this never runs alongside a
// gameplay transport. Open GATT (no bonding) so the web page connects without pairing.
//
// GATT contract, keep in lockstep with web/app.js:
//   service  5f1d0000-7c5a-4e2a-9b6e-2a8f3c9d1e00
//   INFO     5f1d0001  read + notify   : JSON {name,fw,build,slot,batt,ident,transport,profile,
//                                              dirmode,profiles[2][],dirmodes[2][]}
//   CMD      5f1d0002  write           : JSON {transport|profile|dirmode:int} or {action:"reboot|forget"}
//   OTACTL   5f1d0003  write + notify  : control [op,...] / status [code,...]
//   OTADATA  5f1d0004  write-no-resp   : raw firmware chunks
//
// OTA control opcodes (client->dev): 0x01 BEGIN [u32 size][u32 crc], 0x02 END, 0x03 ABORT.
// OTA status codes (dev->client):    0x10 READY [u32 chunk][u32 window], 0x11 ACK [u32 received],
//                                     0x12 DONE, 0x1f ERROR [u8 code][msg].

#include "bt_config.hpp"
#include "settings.hpp"
#include "battery.hpp"
#include "board_config.h"
#include "bt_transport.hpp"
#include "nes_controller.hpp"

#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "driver/gpio.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "cJSON.h"

#define FW_VERSION "1.0.0"

static const char* TAG = "bt_config";

namespace {

// --- 128-bit UUIDs (little-endian byte order; index 12 selects the characteristic) -------------
#define CFG_UUID128(dd) {0x00,0x1e,0x9d,0x3c,0x8f,0x2a,0x6e,0x9b,0x2a,0x4e,0x5a,0x7c,(dd),0x00,0x1d,0x5f}
uint8_t uuid_svc[16]     = CFG_UUID128(0x00);
uint8_t uuid_info[16]    = CFG_UUID128(0x01);
uint8_t uuid_cmd[16]     = CFG_UUID128(0x02);
uint8_t uuid_otactl[16]  = CFG_UUID128(0x03);
uint8_t uuid_otadata[16] = CFG_UUID128(0x04);

// --- OTA protocol constants --------------------------------------------------------------------
constexpr uint8_t OP_BEGIN = 0x01, OP_END = 0x02, OP_ABORT = 0x03;
constexpr uint8_t ST_READY = 0x10, ST_ACK = 0x11, ST_DONE = 0x12, ST_ERROR = 0x1f;
constexpr uint32_t OTA_WINDOW   = 8192;   // bytes the client may send before an ACK
constexpr uint32_t OTA_CHUNK_MAX = 500;   // capped per-write payload (also bounded by MTU)

// --- GATT attribute table indices --------------------------------------------------------------
enum {
    IDX_SVC,
    IDX_INFO_CHAR, IDX_INFO_VAL, IDX_INFO_CCC,
    IDX_CMD_CHAR,  IDX_CMD_VAL,
    IDX_OTACTL_CHAR, IDX_OTACTL_VAL, IDX_OTACTL_CCC,
    IDX_OTADATA_CHAR, IDX_OTADATA_VAL,
    IDX_NB,
};
uint16_t s_handles[IDX_NB] = {0};

// --- runtime state -----------------------------------------------------------------------------
esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
uint16_t s_conn_id = 0xffff;
bool     s_connected = false;
uint16_t s_mtu = 23;
int64_t  s_last_activity_ms = 0;

// OTA transfer state
esp_ota_handle_t s_ota = 0;
const esp_partition_t* s_ota_part = nullptr;
bool     s_ota_active = false;
uint32_t s_ota_size = 0, s_ota_recv = 0, s_ota_acked = 0, s_ota_expect_crc = 0, s_ota_crc = 0;
int64_t  s_ota_last_data_ms = 0;   // for the stall watchdog (so a hung OTA can't trap config mode)

static inline int64_t now_ms() { return esp_timer_get_time() / 1000; }
static inline void touch() { s_last_activity_ms = now_ms(); }

// --- LED (blue blink = config mode; harmless if unpopulated) -----------------------------------
void led_config_on(bool on) { gpio_set_level((gpio_num_t)LED_BLUE, on ? 0 : 1); }

// --- INFO JSON ---------------------------------------------------------------------------------
char s_info_buf[480];
uint16_t s_info_len = 0;

void build_info_json() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "NES Advantage Config");
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddStringToObject(root, "build", __DATE__ " " __TIME__);
    cJSON_AddStringToObject(root, "slot", running ? running->label : "?");
    cJSON_AddNumberToObject(root, "batt", battery::present() ? battery::level_percent() : -1);
    cJSON_AddNumberToObject(root, "ident", settings::identity_generation());
    cJSON_AddNumberToObject(root, "transport", settings::transport());
    cJSON_AddNumberToObject(root, "profile", settings::profile());
    cJSON_AddNumberToObject(root, "dirmode", settings::directional_mode());

    // Per-transport profile + directional-mode names, from the gameplay ops tables (pure table
    // lookups, safe to call without bringing a transport up).
    const bt::Ops* ops[2] = { bt_pro_ops(), bt_ble_ops() };
    cJSON* profiles = cJSON_AddArrayToObject(root, "profiles");
    cJSON* dirmodes = cJSON_AddArrayToObject(root, "dirmodes");
    for (int t = 0; t < 2; t++) {
        cJSON* pl = cJSON_CreateArray();
        for (uint8_t i = 0; i < ops[t]->num_profiles(); i++)
            cJSON_AddItemToArray(pl, cJSON_CreateString(ops[t]->profile_name(i)));
        cJSON_AddItemToArray(profiles, pl);
        cJSON* dl = cJSON_CreateArray();
        for (uint8_t i = 0; i < ops[t]->num_directional_modes(); i++)
            cJSON_AddItemToArray(dl, cJSON_CreateString(ops[t]->directional_mode_name(i)));
        cJSON_AddItemToArray(dirmodes, dl);
    }

    char* s = cJSON_PrintUnformatted(root);
    s_info_len = 0;
    if (s) {
        size_t n = strlen(s);
        if (n >= sizeof(s_info_buf)) n = sizeof(s_info_buf) - 1;
        memcpy(s_info_buf, s, n);
        s_info_buf[n] = '\0';
        s_info_len = (uint16_t)n;
        cJSON_free(s);
    }
    cJSON_Delete(root);
}

void publish_info(bool notify) {
    build_info_json();
    if (s_handles[IDX_INFO_VAL])
        esp_ble_gatts_set_attr_value(s_handles[IDX_INFO_VAL], s_info_len, (uint8_t*)s_info_buf);
    if (notify && s_connected && s_gatts_if != ESP_GATT_IF_NONE)
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_INFO_VAL],
                                    s_info_len, (uint8_t*)s_info_buf, false);
}

// --- OTA status notifications ------------------------------------------------------------------
void ota_notify(const uint8_t* buf, uint16_t len) {
    if (s_connected && s_gatts_if != ESP_GATT_IF_NONE)
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_OTACTL_VAL],
                                    len, (uint8_t*)buf, false);
}
void ota_error(uint8_t code, const char* msg) {
    uint8_t b[40]; b[0] = ST_ERROR; b[1] = code;
    uint16_t n = 2;
    if (msg) { uint16_t m = strlen(msg); if (m > sizeof(b) - 2) m = sizeof(b) - 2; memcpy(b + 2, msg, m); n += m; }
    ota_notify(b, n);
    ESP_LOGE(TAG, "OTA error %u: %s", code, msg ? msg : "");
}
void ota_abort_cleanup() {
    if (s_ota_active && s_ota) esp_ota_abort(s_ota);
    s_ota_active = false; s_ota = 0; s_ota_part = nullptr;
    s_ota_size = s_ota_recv = s_ota_acked = 0;
}

void put_u32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
uint32_t get_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void ota_begin(const uint8_t* val, uint16_t len) {
    if (len < 9) { ota_error(1, "short begin"); return; }
    ota_abort_cleanup();
    s_ota_size = get_u32(val + 1);
    s_ota_expect_crc = get_u32(val + 5);
    s_ota_part = esp_ota_get_next_update_partition(nullptr);
    if (!s_ota_part) { ota_error(2, "no ota slot"); return; }
    ESP_LOGI(TAG, "OTA begin: %u bytes -> %s (erasing)", (unsigned)s_ota_size, s_ota_part->label);
    esp_err_t e = esp_ota_begin(s_ota_part, s_ota_size, &s_ota);
    if (e != ESP_OK) { ota_error(3, esp_err_to_name(e)); return; }
    s_ota_active = true; s_ota_recv = 0; s_ota_acked = 0; s_ota_crc = 0;
    s_ota_last_data_ms = now_ms();
    uint32_t chunk = s_mtu > 3 ? (uint32_t)(s_mtu - 3) : 20;
    if (chunk > OTA_CHUNK_MAX) chunk = OTA_CHUNK_MAX;
    uint8_t b[9]; b[0] = ST_READY; put_u32(b + 1, chunk); put_u32(b + 5, OTA_WINDOW);
    ota_notify(b, 9);
    ESP_LOGI(TAG, "OTA ready: chunk=%u window=%u", (unsigned)chunk, (unsigned)OTA_WINDOW);
}

void ota_data(const uint8_t* val, uint16_t len) {
    if (!s_ota_active) return;
    esp_err_t e = esp_ota_write(s_ota, val, len);
    if (e != ESP_OK) { ota_error(4, esp_err_to_name(e)); ota_abort_cleanup(); return; }
    s_ota_crc = esp_rom_crc32_le(s_ota_crc, val, len);
    s_ota_recv += len;
    s_ota_last_data_ms = now_ms();
    if (s_ota_recv - s_ota_acked >= OTA_WINDOW / 2 || s_ota_recv >= s_ota_size) {
        s_ota_acked = s_ota_recv;
        uint8_t b[5]; b[0] = ST_ACK; put_u32(b + 1, s_ota_acked);
        ota_notify(b, 5);
    }
}

void ota_end() {
    if (!s_ota_active) { ota_error(5, "not active"); return; }
    if (s_ota_recv != s_ota_size)
        ESP_LOGW(TAG, "OTA size mismatch: got %u want %u", (unsigned)s_ota_recv, (unsigned)s_ota_size);
    if (s_ota_crc != s_ota_expect_crc)
        ESP_LOGW(TAG, "OTA crc32 0x%08x != expected 0x%08x (SHA still enforced)",
                 (unsigned)s_ota_crc, (unsigned)s_ota_expect_crc);
    esp_err_t e = esp_ota_end(s_ota);          // validates the appended SHA-256 / image header
    s_ota = 0;
    if (e != ESP_OK) { s_ota_active = false; ota_error(6, esp_err_to_name(e)); return; }
    e = esp_ota_set_boot_partition(s_ota_part);
    if (e != ESP_OK) { s_ota_active = false; ota_error(7, esp_err_to_name(e)); return; }
    s_ota_active = false;
    ESP_LOGW(TAG, "OTA complete -> boot %s; rebooting", s_ota_part->label);
    uint8_t b = ST_DONE; ota_notify(&b, 1);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

// --- CMD (settings) ----------------------------------------------------------------------------
void do_forget() {
    ESP_LOGW(TAG, "forget: remove BLE bonds + rotate identity + reboot");
    int n = esp_ble_get_bond_device_num();
    if (n > 0) {
        esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
        if (list && esp_ble_get_bond_device_list(&n, list) == ESP_OK)
            for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
        free(list);
    }
    settings::bump_identity_generation();
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

void handle_cmd(const uint8_t* val, uint16_t len) {
    cJSON* j = cJSON_ParseWithLength((const char*)val, len);
    if (!j) { ESP_LOGW(TAG, "bad CMD json"); return; }

    cJSON* it;
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(j, "transport"))) {
        uint8_t t = (uint8_t)it->valueint; if (t >= bt::TRANSPORT_COUNT) t = 0;
        settings::set_transport(t); ESP_LOGI(TAG, "set transport=%u", t);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(j, "profile"))) {
        settings::set_profile((uint8_t)it->valueint); ESP_LOGI(TAG, "set profile=%d", it->valueint);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(j, "dirmode"))) {
        settings::set_directional_mode((uint8_t)it->valueint); ESP_LOGI(TAG, "set dirmode=%d", it->valueint);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(j, "action"))) {
        const char* a = it->valuestring;
        if (!strcmp(a, "reboot")) { cJSON_Delete(j); ESP_LOGW(TAG, "reboot"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        if (!strcmp(a, "forget")) { cJSON_Delete(j); do_forget(); }
    }
    cJSON_Delete(j);
    publish_info(true);     // reflect persisted state back to the UI
}

// --- GATT attribute table ----------------------------------------------------------------------
const uint16_t PRIMARY_SERVICE_UUID = ESP_GATT_UUID_PRI_SERVICE;
const uint16_t CHAR_DECLARE_UUID    = ESP_GATT_UUID_CHAR_DECLARE;
const uint16_t CHAR_CCC_UUID        = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
const uint8_t  PROP_READ_NOTIFY     = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
const uint8_t  PROP_WRITE           = ESP_GATT_CHAR_PROP_BIT_WRITE;
const uint8_t  PROP_WRITE_NOTIFY    = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
const uint8_t  PROP_WRITE_NR        = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
uint8_t ccc_info[2] = {0, 0};
uint8_t ccc_otactl[2] = {0, 0};
uint8_t dummy = 0;

// Ordered to match the IDX_* enum exactly (no C99 array designators, not portable in C++).
const esp_gatts_attr_db_t s_attr_db[IDX_NB] = {
    // IDX_SVC, primary service
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&PRIMARY_SERVICE_UUID,
        ESP_GATT_PERM_READ, sizeof(uuid_svc), sizeof(uuid_svc), uuid_svc}},

    // IDX_INFO_CHAR / VAL / CCC, read + notify
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_DECLARE_UUID,
        ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&PROP_READ_NOTIFY}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, uuid_info,
        ESP_GATT_PERM_READ, sizeof(s_info_buf), 1, (uint8_t*)&dummy}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_CCC_UUID,
        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(ccc_info), sizeof(ccc_info), ccc_info}},

    // IDX_CMD_CHAR / VAL, write
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_DECLARE_UUID,
        ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&PROP_WRITE}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, uuid_cmd,
        ESP_GATT_PERM_WRITE, 256, 1, (uint8_t*)&dummy}},

    // IDX_OTACTL_CHAR / VAL / CCC, write + notify
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_DECLARE_UUID,
        ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&PROP_WRITE_NOTIFY}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, uuid_otactl,
        ESP_GATT_PERM_WRITE, 64, 1, (uint8_t*)&dummy}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_CCC_UUID,
        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(ccc_otactl), sizeof(ccc_otactl), ccc_otactl}},

    // IDX_OTADATA_CHAR / VAL, write without response
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&CHAR_DECLARE_UUID,
        ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&PROP_WRITE_NR}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, uuid_otadata,
        ESP_GATT_PERM_WRITE, 512, 1, (uint8_t*)&dummy}},
};

// --- advertising -------------------------------------------------------------------------------
esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false, .include_name = false, .include_txpower = true,
    .min_interval = 0x0006, .max_interval = 0x0010, .appearance = 0,
    .manufacturer_len = 0, .p_manufacturer_data = nullptr,
    .service_data_len = 0, .p_service_data = nullptr,
    .service_uuid_len = sizeof(uuid_svc), .p_service_uuid = uuid_svc,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
esp_ble_adv_data_t s_scan_rsp = {
    .set_scan_rsp = true, .include_name = true, .include_txpower = false,
    .min_interval = 0, .max_interval = 0, .appearance = 0,
    .manufacturer_len = 0, .p_manufacturer_data = nullptr,
    .service_data_len = 0, .p_service_data = nullptr,
    .service_uuid_len = 0, .p_service_uuid = nullptr, .flag = 0,
};
esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20, .adv_int_max = 0x40, .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
bool s_adv_data_done = false, s_scan_rsp_done = false;

void start_adv_when_ready() {
    if (s_adv_data_done && s_scan_rsp_done) esp_ble_gap_start_advertising(&s_adv_params);
}

// --- GAP / GATTS event handlers ----------------------------------------------------------------
void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: s_adv_data_done = true; start_adv_when_ready(); break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT: s_scan_rsp_done = true; start_adv_when_ready(); break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            ESP_LOGI(TAG, "advertising as \"NES Advantage Config\"");
        else ESP_LOGE(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
        break;
    default: break;
    }
}

void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name("NES Advantage Config");
        esp_ble_gatts_create_attr_tab(s_attr_db, gatts_if, IDX_NB, 0);
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK || param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "attr tab failed (status=%d num=%d)",
                     param->add_attr_tab.status, param->add_attr_tab.num_handle);
            break;
        }
        memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
        esp_ble_gatts_start_service(s_handles[IDX_SVC]);
        publish_info(false);                       // seed INFO value for reads
        esp_ble_gap_config_adv_data(&s_adv_data);
        esp_ble_gap_config_adv_data(&s_scan_rsp);
        break;
    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        touch();
        ESP_LOGI(TAG, "client connected (conn=%d)", s_conn_id);
        break;
    case ESP_GATTS_MTU_EVT:
        s_mtu = param->mtu.mtu;
        ESP_LOGI(TAG, "MTU = %u", s_mtu);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGW(TAG, "client disconnected (reason=0x%x) -> re-advertise", param->disconnect.reason);
        s_connected = false; s_conn_id = 0xffff;
        ota_abort_cleanup();
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GATTS_WRITE_EVT: {
        if (param->write.is_prep) break;           // chunks are small single writes; no long-write
        touch();
        uint16_t h = param->write.handle;
        if (h == s_handles[IDX_CMD_VAL])          handle_cmd(param->write.value, param->write.len);
        else if (h == s_handles[IDX_OTADATA_VAL]) ota_data(param->write.value, param->write.len);
        else if (h == s_handles[IDX_OTACTL_VAL]) {
            uint8_t op = param->write.len ? param->write.value[0] : 0;
            if (op == OP_BEGIN)      ota_begin(param->write.value, param->write.len);
            else if (op == OP_END)   ota_end();
            else if (op == OP_ABORT) { ota_abort_cleanup(); ESP_LOGW(TAG, "OTA aborted by client"); }
        }
        break;
    }
    default: break;
    }
}

}  // namespace

namespace config {

void run() {
    ESP_LOGW(TAG, "=== BLE CONFIG / OTA MODE === fw %s (%s)", FW_VERSION, __DATE__ " " __TIME__);
    led_config_on(true);

    // Use a BT address distinct from every gameplay identity so a host's per-address GATT cache can
    // never confuse this config service with the BLE gameplay HID service (which shares the factory
    // MAC). Without this, BlueZ/Chrome reuse the cached HID handle map here and reads land on the
    // write-only characteristics -> GATT_READ_NOT_PERMIT and the connection fails. Gameplay identity
    // rotation only ever varies mac[5], so flipping mac[4] keeps the config address permanently apart.
    // Must run before esp_bt_controller_init (the controller derives its address from the base MAC).
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        mac[4] ^= 0xC0;
        esp_base_mac_addr_set(mac);
        ESP_LOGI(TAG, "config BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

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

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));
    esp_ble_gatt_set_local_mtu(517);

    // Escape hatch: poll the controller so the user can leave config mode without a web client.
    // Holding Start for 3 s reboots to gameplay (Start is not part of the A+B+Select entry gesture,
    // so a lingering entry hold can't trigger it). A power-cycle also exits; config mode is a
    // one-shot armed only by the gesture's software reset, so any cold boot returns to gameplay.
    NESController nes(NES_CLK_P1, NES_CLK_P2, NES_LATCH, NES_DATA_P1, NES_DATA_P2);
    nes.begin();
    const int64_t kEscapeHoldMs = 3'000;
    int64_t start_held_since = 0;

    s_last_activity_ms = now_ms();
    const int64_t kIdleTimeoutMs = 300'000;        // 5 min with no activity -> back to gameplay

    const int64_t kOtaStallMs = 15'000;            // abort a hung transfer so the escapes re-enable
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        // OTA stall watchdog: a wedged transfer must never permanently disable the escapes below
        // (there is no accessible power/reset button on the production board).
        if (s_ota_active && now_ms() - s_ota_last_data_ms > kOtaStallMs) {
            ESP_LOGW(TAG, "OTA stalled %llds -> abort", (long long)(kOtaStallMs / 1000));
            ota_error(8, "stalled");
            ota_abort_cleanup();
        }

        // On-controller escape: Start held ~3 s -> back to gameplay (skip during an OTA write).
        // Check only the ACTIVE player: the deselected player's line reads all-8-high (every button
        // looks pressed, that's how read() senses the player-select switch), so testing both players
        // would see a phantom Start and bail out instantly.
        if (!s_ota_active) {
            nes.read();
            bool start = nes.getButtonState(nes.getPlayerSelection(), NESController::BUTTON_START);
            if (start) {
                if (start_held_since == 0) start_held_since = now_ms();
                else if (now_ms() - start_held_since > kEscapeHoldMs) {
                    ESP_LOGW(TAG, "Start held -> exit config mode to gameplay");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
            } else {
                start_held_since = 0;
            }
        }

        if (!s_connected) led_config_on((now_ms() / 250) % 2 == 0);   // blink while waiting
        else              led_config_on(true);                        // solid while connected

        // Don't time out mid-OTA; otherwise return to gameplay after idle.
        if (!s_ota_active && now_ms() - s_last_activity_ms > kIdleTimeoutMs) {
            ESP_LOGW(TAG, "config mode idle %llds -> reboot to gameplay", (long long)(kIdleTimeoutMs / 1000));
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

}  // namespace config