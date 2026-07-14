// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "console.hpp"

#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "linenoise/linenoise.h"

#include "app_control.hpp"
#include "battery.hpp"
#include "board_config.h"
#include "bt_transport.hpp"
#include "settings.hpp"

#if defined(CONFIG_BT_HCI_LOG_DEBUG_EN)
#include "hci_log/bt_hci_log.h"
#endif

static const char* TAG = "console";

// `batt`: VBAT + coarse SoC + charge status (settled read, ~300 ms).
static int cmd_batt(int, char**) {
    int mv = battery::read_mv();
    if (mv < 0) { printf("battery: sense unavailable\n"); return 1; }
    printf("VBAT=%d mV  pct~%u%%  present=%d charging=%d full=%d\n",
           mv, battery::level_percent(), battery::present(),
           battery::is_charging(), battery::is_full());
    return 0;
}

// `adc`: raw counts + sense mV + computed VBAT. Use it to calibrate BATT_CAL_PERMILLE against a
// meter reading on +BATT: cal = 1000 * meter_mV / computed_VBAT.
static int cmd_adc(int, char**) {
    int raw = battery::read_raw();
    int sense = battery::read_sense_mv();
    int vbat = battery::read_mv();
    printf("raw=%d  sense=%d mV  VBAT=%d mV  (measure +BATT, cal=1000*meter/VBAT)\n",
           raw, sense, vbat);
    return 0;
}

// `led r|g|b|all off|on`: drive an LED directly (active-low). Takes the LEDs away from the auto
// state display until `led auto`. Used to bench-check indicators (e.g. the green charge LED).
static int cmd_led(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "auto") == 0) {
        app::set_led_auto(true);
        printf("LEDs: auto (link/battery state)\n");
        return 0;
    }
    if (argc != 3) { printf("usage: led r|g|b|all on|off  |  led auto\n"); return 1; }
    bool on = strcmp(argv[2], "on") == 0;
    if (!on && strcmp(argv[2], "off") != 0) { printf("usage: led ... on|off\n"); return 1; }
    int lvl = on ? 0 : 1;   // active-low
    app::set_led_auto(false);
    const char* c = argv[1];
    bool all = strcmp(c, "all") == 0;
    if (all || strcmp(c, "r") == 0) gpio_set_level((gpio_num_t)LED_RED,   lvl);
    if (all || strcmp(c, "g") == 0) gpio_set_level((gpio_num_t)LED_GREEN, lvl);
    if (all || strcmp(c, "b") == 0) gpio_set_level((gpio_num_t)LED_BLUE,  lvl);
    printf("LED %s -> %s\n", c, on ? "on" : "off");
    return 0;
}

// `get`: dump runtime state.
static int cmd_get(int, char**) {
    const char* link = "idle";
    switch (bt::link_state()) {
        case bt::LINK_ADVERTISING: link = "advertising"; break;
        case bt::LINK_CONNECTED:   link = "connected";   break;
        default: break;
    }
    printf("transport=%s  profile=%u(%s)  dirmode=%u(%s)  player=P%u  identity_gen=%u\n",
           bt::transport_name((bt::Transport)settings::transport()),
           app::profile() + 1, bt::profile_name(app::profile()),
           app::directional_mode() + 1, bt::directional_mode_name(app::directional_mode()),
           app::player() + 1, settings::identity_generation());
    printf("link=%s  battery: present=%d pct=%u charging=%d full=%d\n",
           link, battery::present(), battery::level_percent(),
           battery::is_charging(), battery::is_full());
    return 0;
}

// `profile <n>`: 1-based button profile, applied live.
static int cmd_profile(int argc, char** argv) {
    if (argc != 2) { printf("usage: profile <1..%u>\n", bt::num_profiles()); return 1; }
    int n = atoi(argv[1]);
    if (n < 1 || n > bt::num_profiles()) { printf("out of range 1..%u\n", bt::num_profiles()); return 1; }
    app::set_profile((uint8_t)(n - 1));
    printf("profile -> %d (%s)\n", n, bt::profile_name(n - 1));
    return 0;
}

// `dirmode <n>`: 1-based directional mode, applied live.
static int cmd_dirmode(int argc, char** argv) {
    if (argc != 2) { printf("usage: dirmode <1..%u>\n", bt::num_directional_modes()); return 1; }
    int n = atoi(argv[1]);
    if (n < 1 || n > bt::num_directional_modes()) {
        printf("out of range 1..%u\n", bt::num_directional_modes()); return 1;
    }
    app::set_directional_mode((uint8_t)(n - 1));
    printf("dirmode -> %d (%s)\n", n, bt::directional_mode_name(n - 1));
    return 0;
}

// `transport classic|ble`: set the active transport and reboot into it.
static int cmd_transport(int argc, char** argv) {
    if (argc != 2) { printf("usage: transport classic|ble\n"); return 1; }
    int t;
    if      (strcmp(argv[1], "classic") == 0) t = bt::TRANSPORT_CLASSIC;
    else if (strcmp(argv[1], "ble") == 0)     t = bt::TRANSPORT_BLE;
    else { printf("usage: transport classic|ble\n"); return 1; }
    printf("transport -> %s; rebooting\n", bt::transport_name((bt::Transport)t));
    settings::set_transport((uint8_t)t);
    esp_restart();
    return 0;
}

// `forget`: clear bonds, rotate BT identity, reboot (re-pair from scratch).
static int cmd_forget(int, char**) {
    printf("forget host / rotate identity; rebooting\n");
    bt::forget_host();
    return 0;
}

// `config`: reboot into BLE config/OTA mode (same as the A+B+Select hold gesture). Handy for bench
// work so config mode can be entered over serial without touching the controller.
static int cmd_config(int, char**) {
    printf("entering BLE config/OTA mode; rebooting\n");
    app::enter_config_mode();
    return 0;
}

// `sleep`: force deep sleep (wake by holding Start).
static int cmd_sleep(int, char**) {
    printf("entering deep sleep; hold Start to wake\n");
    app::request_sleep();
    return 0;
}

// `autosleep on|off`: on (default) = normal idle/disconnect auto-sleep; off = stay awake for bench
// tests. Runtime only, resets to on at boot. `sleep` (manual) and the Start gesture still work.
static int cmd_autosleep(int argc, char** argv) {
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: autosleep on|off\n"); return 1;
    }
    bool on = strcmp(argv[1], "on") == 0;
    app::set_sleep_inhibit(!on);
    printf("auto-sleep %s\n", on ? "on" : "OFF (bench; stays awake)");
    return 0;
}

// `reboot`: restart the firmware.
static int cmd_reboot(int, char**) {
    printf("rebooting\n");
    esp_restart();
    return 0;
}

// `heap`: free-heap readout.
#if defined(CONFIG_BT_HCI_LOG_DEBUG_EN)
// `hcilog`: dump the raw HCI packet ring buffer (bench debug builds only).
static int cmd_hcilog(int, char**) {
    bt_hci_log_hci_data_show();
    return 0;
}
#endif

static int cmd_heap(int, char**) {
    printf("heap: free=%u min_ever=%u\n",
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

static void reg(const char* command, const char* help, esp_console_cmd_func_t func) {
    esp_console_cmd_t cmd = {};   // value-init zeroes argtable/context/func_w_context
    cmd.command = command;
    cmd.help = help;
    cmd.func = func;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_cmds() {
    reg("batt",      "Battery: VBAT mV + SoC + charge status",         cmd_batt);
    reg("adc",       "Raw battery ADC read (calibration)",             cmd_adc);
    reg("led",       "Drive an LED: led r|g|b|all on|off | led auto",  cmd_led);
    reg("get",       "Dump runtime state",                            cmd_get);
    reg("profile",   "Set button profile (live): profile <n>",         cmd_profile);
    reg("dirmode",   "Set directional mode (live): dirmode <n>",       cmd_dirmode);
    reg("transport", "Set transport + reboot: transport classic|ble",  cmd_transport);
    reg("config",    "Reboot into BLE config/OTA mode",                cmd_config);
    reg("forget",    "Forget host + rotate identity + reboot",         cmd_forget);
    reg("sleep",     "Force deep sleep (wake by holding Start)",        cmd_sleep);
    reg("autosleep", "Idle/disconnect auto-sleep: autosleep on|off",    cmd_autosleep);
    reg("reboot",    "Restart the firmware",                          cmd_reboot);
    reg("heap",      "Free-heap readout",                             cmd_heap);
#if defined(CONFIG_BT_HCI_LOG_DEBUG_EN)
    reg("hcilog",    "Dump raw HCI packet ring buffer (debug builds)", cmd_hcilog);
#endif
}

esp_err_t console_start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "nesadv>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) { ESP_LOGE(TAG, "repl_uart: %s", esp_err_to_name(err)); return err; }

    esp_console_register_help_command();
    register_cmds();

    // Without a real TTY, IDF linenoise runs "dumb" and an empty/EOF read returns an empty line, so
    // a detached host makes the REPL spin and flood UART. Disabling allow-empty makes EOF return
    // NULL and the task exits cleanly on detach (pressing Enter on an empty line also exits; reset
    // to get it back). Fine for a bench tool driven by `serial_proxy.py send`.
    linenoiseAllowEmpty(false);

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) { ESP_LOGE(TAG, "start_repl: %s", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "console ready - type 'help'");
    return ESP_OK;
}
