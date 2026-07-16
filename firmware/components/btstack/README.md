# BTstack (vendored subset)

Pinned copy of [BTstack](https://github.com/bluekitchen/btstack) **v1.8.2** (tag, 2026-06-15),
used as the Bluetooth Classic host stack for the Switch Pro Controller transport
(`main/bt_pro_btstack.cpp`). The BLE gameplay transport and the config/OTA boot mode remain on
ESP-IDF Bluedroid; exactly one host stack is initialized per boot on the shared controller
(VHCI). Motivation and bring-up notes: `docs/switch_pro_protocol.md` and the project history —
Bluedroid's inbound pre-pairing behavior is rejected by the Switch 2 (drop reason 0x05 before
SSP) and is not fixable from the app; BTstack-based Pro emulators are known to pair.

## Layout

- `upstream/` — unmodified files from the v1.8.2 tag (subset: HCI/L2CAP/SDP/HID-device core,
  FreeRTOS run loop, stdout HCI dump; all headers). To add a file the linker wants, copy it
  from the same tag.
- `port/` — from upstream `port/esp32/components/btstack/`, with one local change:
  `hci_transport_esp32_vhci.c` enables the controller in `ESP_BT_MODE_CLASSIC_BT` (not BTDM)
  when built Classic-only, because the transport boot releases the BLE controller memory first
  (marked `// btna:`).
- `include/btstack_config.h` — ours (Classic-only, quiet-slave L2CAP defaults, NVS link keys).
- `CMakeLists.txt` — ours.

## License

BTstack is Copyright BlueKitchen GmbH, free for personal/non-commercial use only (see the
header in any upstream file; clause 4 forbids commercial use). This project is a personal
open-source hardware mod, which fits. A commercial fork of this project would need a BTstack
license from BlueKitchen (or a different stack).
