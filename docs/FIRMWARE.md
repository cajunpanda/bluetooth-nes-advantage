# Firmware

ESP32-WROOM-32E firmware. Native ESP-IDF v5 with Bluedroid dual-mode Bluetooth: the Switch Pro
Controller emulation needs BT Classic, which NimBLE cannot do. Built with PlatformIO or plain
`idf.py`. Source is in [`../firmware/`](../firmware/).

## Building from source

Requirements: Python 3 and [PlatformIO](https://platformio.org/) (CLI or the VSCode extension).

```bash
cd firmware
pio run -e wroom32              # build
pio run -e wroom32 -t upload    # flash over the Tag-Connect cable
pio device monitor              # serial console, 115200 baud
```

Plain ESP-IDF works too:

```bash
cd firmware
idf.py set-target esp32
idf.py build flash monitor
```

A build is the main static check; there is no unit-test suite. Treat warnings in `main/*.cpp` as
regressions. If the board already runs firmware, you can skip the cable and flash your build's
`firmware/.pio/build/wroom32/firmware.bin` over the air from the config page.

## Shared serial monitor

Only one process can own the serial port, which is awkward when a person and an automated tool both
want to watch it. **benchmux** (a standalone bench serial proxy, `serial_proxy.py`) owns the port
and tees everything to a shared logfile, and its `flash` command coordinates the port handoff so the
monitor never has to be stopped by hand. Run it by path or symlinked onto `PATH`:

```bash
serial_proxy.py monitor --port /dev/ttyUSB0                       # start once; tees to /tmp/serial_proxy.log
tail -f /tmp/serial_proxy.log                                     # watch live, any number of readers
serial_proxy.py flash --flash-cmd 'pio run -e wroom32 -t upload'  # pause proxy, build + upload, resume + reset
```

`pio device monitor` needs a real TTY; reading the proxy's logfile is the scriptable path. See
[`../tools/README.md`](../tools/README.md).

## First flash (blank board)

A freshly fabbed board has no firmware, so the first flash goes over the Tag-Connect serial cable
(see [HARDWARE.md](HARDWARE.md)). After this one flash, use the browser path for all updates.

1. Install esptool: `pip install esptool`
2. Plug the cable into your PC and press it onto the J4 pads. The cable powers the board and handles
   reset automatically; no buttons needed.
3. Flash the merged image:

   ```bash
   esptool.py --chip esp32 --baud 460800 write_flash 0x0 bt-nes-advantage.bin
   ```

   Or, with separate files:

   ```bash
   esptool.py --chip esp32 --baud 460800 write_flash \
     0x1000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
   ```

4. The blue LED blinks when it boots (pairing mode).

## Updating over the air

The normal update path is the browser, no cable. On the stick, hold A + B + Select for 5 s to
restart into config mode, open the config page in Chrome or Edge, connect to "NES Advantage Config",
drop a `.bin` on the OTA card, and flash. The stick verifies the image and reboots into it. Details
in [`../web/README.md`](../web/README.md).

## Module layout (`main/`)

| File | Role |
|---|---|
| `app_main.cpp` | Entry point and state machine: gestures, sleep, LEDs, player select |
| `nes_controller.{hpp,cpp}` | CD4021 read and player-select |
| `settings.{hpp,cpp}` | NVS settings: transport, profile, directional mode, identity generation |
| `bt_transport.{hpp,cpp}` | Transport-neutral `bt::` API, dispatch to the active transport |
| `bt_pro.cpp` | BT Classic Switch Pro transport and connection state machine |
| `bt_ble.cpp` | BLE HID transport, two HID services (P1/P2) for take-turns play |
| `bt_config.{hpp,cpp}` | BLE config/OTA boot mode: GATT settings service plus `esp_ota` update |
| `power.{hpp,cpp}` | ULP sleep polling, deep-sleep entry, post-wake RTC-GPIO release |
| `battery.{hpp,cpp}` | Battery monitor (ADC divider plus TP4056 status) |
| `board_config.h` | Pin map for the production board (matches [HARDWARE.md](HARDWARE.md)) |

`partitions.csv` defines two 1.75 MB OTA app slots plus `otadata`, so config mode can update
firmware over the air. `sdkconfig.defaults` holds the project Kconfig; key names can drift between
IDF versions, so reconcile with `menuconfig` if you change IDF.

## Pre-build Bluedroid patches

The `wroom32` build runs two small pre-build patches that make Bluedroid's Classic HID device
behave like a real Pro Controller; both edit IDF source with no runtime API, so they must run at
build time, and both are no-ops for BLE.

- `tools/patch_bluedroid_sniff.py`: link-layer power management - a slave that never initiates
  sniff. Required for stable 8BitDo and Switch connections.
- `tools/patch_bluedroid_hid_intr.py`: connection setup - no MITM link-key upgrade (which wedges
  the ESP32 controller's encryption pause/resume), completion of half-open HID connections
  (hosts like the 8BitDo USB Adapter 2 and BlueRetro open only the control channel and expect the
  controller to open the interrupt channel, as a real Pro does), and real-Pro L2CAP MTU (640).
  Required for the 8BitDo USB Adapter 2 (the Switch 2 bridge); see
  `docs/switch_pro_protocol.md` "Connection direction".

## Gestures

One radio is live per boot; the active transport comes from NVS (default Classic). Hold combos for
5 seconds on the active player; the red LED blinks the new selection.

| Combo | Action |
|---|---|
| Start | Deep sleep (the ULP wakes on a button change; hold Start to power back on) |
| Select | Forget host, rotate BT identity, reboot (re-pair or swap host) |
| A+B+Up | Cycle button profile |
| A+B+Down | Cycle directional mode |
| A+B+Select | Reboot into BLE config/OTA mode |
| Select+Start | Toggle transport (Classic / BLE), reboot |

User-facing details (LED meanings, profiles, two-player) are in [MANUAL.md](MANUAL.md).

## Architecture

### Sleep and wake

The NES buttons sit behind CD4021 shift registers, so wake-on-button cannot be a plain GPIO
interrupt; button state is only visible by clocking the register. In deep sleep the ULP coprocessor
pulses LATCH/CLK on RTC GPIOs, shifts in both players' 8 bits, compares against the stored state,
and wakes the main core only on a change. The CPU and radio are fully off between polls. Wake
latency is roughly 30 ms; standby draw is in the microamp range. The firmware wakes fully only if
Start is held; any other button change goes back to sleep.

Pin constraints that follow:

- LATCH/CLK/DATA must be RTC-capable GPIOs. LATCH/CLK use RTC output pins (25/26/27); DATA stays on
  32/33, which have internal pulls (GPIO 34 to 39 are input-only without pulls).
- Battery sense must be ADC1 (GPIO 32 to 39); ADC2 is unusable while the radio is on.
- The DATA pull direction differs by phase (pull-down awake for the player-select sentinel, pull-up
  asleep for standby current), so the pulls are the ESP32 internal ones and the external pull
  footprints (R14/R15) stay unpopulated.
- GPIO12 (MTDI strap) must never be driven.

The pin map is in [`../firmware/main/board_config.h`](../firmware/main/board_config.h) and matches
[HARDWARE.md](HARDWARE.md).

### Transports

- **One radio per boot.** The active transport (Classic or BLE) is an NVS setting; only that
  identity advertises. The Select+Start gesture toggles it and reboots. The two identities (Pro
  Controller vs NES Advantage) are never live at once.
- **Forget rotates the identity.** The hold-Select gesture clears all bonds, XORs the base MAC with
  a persisted generation counter, and reboots. The device returns with a fresh BT address, so a
  forgotten host cannot half-reconnect against stale pairing state.
- **Reconnect vs fresh pair.** Classic stays passive and lets the host open the HID channels; it
  does not device-initiate, even with a stored bond. BLE advertises for the host to connect.

### Config / OTA mode

A+B+Select reboots into a dedicated BLE-only mode (`bt_config.cpp`) serving a small custom GATT
service, so a browser can change settings and flash firmware over the air. The web client is
[`../web/`](../web/). The GATT contract (service `5f1d0000-...`, four characteristics) is
documented at the top of `bt_config.cpp` and mirrored in `web/app.js`; change them together.

- Entry is a one-shot flag in `RTC_NOINIT_ATTR` memory, honored only when the reset reason is a
  software reset. Any cold boot returns to gameplay.
- Config mode advertises on a distinct BT address (factory MAC with one byte flipped). Hosts cache
  GATT layouts per address; without this, a host that had bonded the gameplay HID would reuse the
  stale handle map and fail to connect.
- The production board has no accessible reset button, so every exit is firmware driven: hold Start
  3 s, a 5-minute idle timeout, and an OTA stall watchdog.
- OTA is the standard `esp_ota` dual-slot scheme. Images are SHA-256 verified before the boot
  partition switches; a bad upload leaves the running slot untouched.

### Input latency

| Term | Approach |
|---|---|
| Sample | 1 kHz tick, 2 ms poll loop: about 1 ms average press-to-detect |
| Hand-off | Reports ship on input change (task notify), not on a timer tick |
| Air, BLE | Connection interval pinned at 7.5 ms (the BLE floor on this silicon) |
| Air, Classic | QoS pinned active (t_poll about 10 ms), no sniff during play |

The firmware's own contribution is sub-millisecond on Classic (detect to 0x30 hand-off around
30 us), and pinning the BLE interval at 7.5 ms roughly halves worst-case report wait versus a
host-chosen 15 ms.

### Player select and two-player

One Bluetooth radio can only be one controller to a console receiver, so take-turns play depends on
the host:

| Host | Take-turns | Mechanism |
|---|---|---|
| PC / emulator (BLE) | Yes | Two HID GATT services on one connection; the slider routes the stick to P1's or P2's gamepad |
| BlueRetro (BLE) | Yes | Double-map one controller to both wired ports in BlueRetro config |
| Switch, 8BitDo receiver (Classic) | No | One Pro Controller is one player; the slider picks which |

The BLE implementation registers the same report map twice (two HID services with distinct report
IDs), which is what makes Linux and other hosts enumerate two separate gamepads.

## Protocol reference

The byte-level Switch Pro Controller emulation contract (identity, input/output reports, handshake
order, sniff and connection-direction behavior) is in
[`switch_pro_protocol.md`](switch_pro_protocol.md).
