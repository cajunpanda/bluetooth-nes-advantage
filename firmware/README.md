# Firmware

ESP32-WROOM-32E firmware. Native ESP-IDF v5 with Bluedroid dual-mode Bluetooth: the Switch Pro
Controller emulation needs BT Classic, which NimBLE cannot do. Built with PlatformIO or plain
`idf.py`.

```bash
pio run -e wroom32 -t upload    # build and flash over the Tag-Connect cable
pio device monitor              # serial console, 115200 baud
```

The full guide (toolchain, build variants, module layout, architecture, config/OTA, and the shared
serial monitor) is in [`../docs/FIRMWARE.md`](../docs/FIRMWARE.md). The Switch Pro protocol contract
is in [`../docs/switch_pro_protocol.md`](../docs/switch_pro_protocol.md).
