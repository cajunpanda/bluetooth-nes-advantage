idf_component_register(
    SRCS
        "app_main.cpp"
        "nes_controller.cpp"
        "settings.cpp"
        "bt_transport.cpp"
        "bt_pro_btstack.cpp"
        "bt_ble.cpp"
        "bt_config.cpp"
        "power.cpp"
        "battery.cpp"
        "console.cpp"
    INCLUDE_DIRS "."
    # bt_config.cpp pulls in BLE OTA (app_update) and JSON (vendored cjson component);
    # bt_pro_btstack.cpp pulls in the vendored BTstack component. esp_bt etc. are reached via
    # main's implicit full dependency; these must be requested explicitly.
    REQUIRES app_update cjson btstack)
