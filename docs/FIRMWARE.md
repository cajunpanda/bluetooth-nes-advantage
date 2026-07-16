# Firmware

ESP32-WROOM-32E firmware. Native ESP-IDF v5 with Bluedroid dual-mode Bluetooth: the Switch Pro
Controller emulation needs BT Classic, which NimBLE cannot do. Built with PlatformIO or plain
`idf.py`. Source is in [`../firmware/`](../firmware/).

## Building from source

Requirements: Python 3 and [PlatformIO](https://platformio.org/) (CLI or the VSCode extension). Flash
and monitor through **[benchmux](https://github.com/cajunpanda/benchmux)** (see the next section);
these docs use it for every flash and serial example.

```bash
cd firmware
pio run -e wroom32     # build (plain ESP-IDF works too: idf.py set-target esp32 && idf.py build)
```

A build is the main static check; there is no unit-test suite. Treat warnings in `main/*.cpp` as
regressions. If the board already runs firmware, skip the cable and flash your build's
`firmware/.pio/build/wroom32/firmware.bin` over the air from the config page (drop it on the OTA
card; the stick verifies and reboots into it). See [`../web/README.md`](../web/README.md).

## Serial monitor and flashing (benchmux)

Only one process can own the serial port, which is awkward when a person and an automated tool both
want to watch it. **[benchmux](https://github.com/cajunpanda/benchmux)** (`serial_proxy.py`) owns the
port and tees everything to a shared logfile, and its `flash` command coordinates the port handoff so
the monitor never has to be stopped by hand. It is the recommended way to flash and monitor this
board. Run it by path or symlink it onto `PATH`:

```bash
serial_proxy.py monitor --port /dev/ttyUSB0                       # own the port, tee to /tmp/serial_proxy.log
tail -f /tmp/serial_proxy.log                                     # watch live, any number of readers
serial_proxy.py flash --flash-cmd 'pio run -e wroom32 -t upload'  # build + upload without stopping the monitor
```

`pio device monitor` needs a real TTY; reading the proxy's logfile is the scriptable path. `ble_proxy.py`
does the same over BLE (console + OTA) with no cable. See [`../tools/README.md`](../tools/README.md).

## First flash (blank board)

A freshly fabbed board has no firmware, so the first flash goes over the Tag-Connect serial cable
(see [HARDWARE.md](HARDWARE.md)); after it, all updates are over the air. Install esptool
(`pip install esptool`), press the cable onto the J4 pads (it powers the board and auto-resets, no
buttons), then flash the merged image through benchmux:

```bash
serial_proxy.py monitor --port /dev/ttyUSB0
serial_proxy.py flash --flash-cmd 'esptool.py --chip esp32 --baud 460800 write_flash 0x0 bt-nes-advantage.bin'
```

The blue LED blinks on boot (pairing mode). To flash the separate images instead of the merged
`bt-nes-advantage.bin`, pass them to the same `write_flash`: `0x1000 bootloader.bin 0x8000
partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin`.

## Module layout (`main/`)

| File | Role |
|---|---|
| `app_main.cpp` | Entry point and state machine: gestures, sleep, LEDs, player select |
| `nes_controller.{hpp,cpp}` | CD4021 read and player-select |
| `settings.{hpp,cpp}` | NVS settings: transport, profile, directional mode, identity generation |
| `bt_transport.{hpp,cpp}` | Transport-neutral `bt::` API, dispatch to the active transport |
| `bt_pro_btstack.cpp` | BT Classic Switch Pro transport (BTstack) and connection state machine |
| `bt_ble.cpp` | BLE HID transport, two HID services (P1/P2) for take-turns play |
| `bt_config.{hpp,cpp}` | BLE config/OTA boot mode: GATT settings service plus `esp_ota` update |
| `power.{hpp,cpp}` | ULP sleep polling, deep-sleep entry, post-wake RTC-GPIO release |
| `battery.{hpp,cpp}` | Battery monitor (ADC divider plus TP4056 status) |
| `board_config.h` | Pin map for the production board (matches [HARDWARE.md](HARDWARE.md)) |

`partitions.csv` defines two 1.75 MB OTA app slots plus `otadata`, so config mode can update
firmware over the air. `sdkconfig.defaults` holds the project Kconfig; key names can drift between
IDF versions, so reconcile with `menuconfig` if you change IDF.

## Two Bluetooth host stacks

One controller, two hosts, one of each per boot - selected by the transport setting:

- **Classic (Switch Pro)** runs on **BTstack**, vendored in `firmware/components/btstack`
  (see its README for the pinned version, what is vendored, and the license). BTstack ships a real
  HID *device* profile, so the Pro emulation is plain library calls: on Bluedroid the same thing
  needed five build-time patches to its own source (`tools/patch_bluedroid_*.py`) to bend the
  inbound HID path, the Class-of-Device handling, and Secure Connections support into shape. Moving
  to BTstack deleted all five, dropped ~80 KB of flash, and matches the architecture the other
  known-good ESP32 Pro emulations use.

  Note for anyone reading the git history: this transport was written during a Switch 2 pairing
  investigation, and the commit messages of the era claim Bluedroid *cannot* pair with a Switch 2.
  **That was wrong** - the console was in a wedged state that rejects every device, cleared by a
  power cycle (see `history/switch2-support.md`). Bluedroid would very likely pair too. The reasons
  above are the real ones; do not repeat the Switch 2 claim.
- **BLE** (gameplay transport and the config/OTA boot mode) stays on **Bluedroid**.

Bluedroid's Classic half is therefore compiled out (`CONFIG_BT_CLASSIC_ENABLED=n`), which also
removes a `sdp_init` symbol clash with BTstack. Earlier builds carried five
`tools/patch_bluedroid_*.py` pre-build patches to bend Bluedroid's Classic HID into shape; they
are gone with it (recoverable from git history before the BTstack switch, if ever needed).

## Architecture

Gestures (hold-combos) are matched in `detect_gesture()` in `app_main.cpp`; the user-facing list is
in [MANUAL.md](MANUAL.md).

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
service, so a browser can change settings, test the controller live, and flash firmware over the
air. The web client is the single-file [`../web/index.html`](../web/index.html). The GATT contract
(service `5f1d0000-...`, seven characteristics: INFO/CMD/OTACTL/OTADATA plus INPUT/LOG/CONSOLE) is
documented at the top of `bt_config.cpp` and mirrored in `web/index.html`; change them together.

- Entry is a one-shot flag in `RTC_NOINIT_ATTR` memory, honored only when the reset reason is a
  software reset. Any cold boot returns to gameplay. The `config` bench-console command arms the
  same flag over serial, so config mode can be entered without the button gesture.
- Boot also enters config mode when no controller is detected. A disconnected NES Advantage reads as
  all-buttons-held (both CD4021 data lines idle-high), which would oscillate player-select and trip
  the Select+Start transport gesture into a reboot loop. `NESController::diagnose()` catches this at
  boot; config mode reports it as `wiring` in the INFO JSON (none/p1/p2/both), blinks the red LED,
  and answers the `diag` console command. The `diag` command works in gameplay too.
- Config mode brings up the battery monitor and streams a live INPUT frame (buttons, player-select,
  turbo rate) plus the device log (LOG), and takes console commands (CONSOLE) whose output it
  echoes back over LOG.
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
| Sample | 1 kHz tick, 2 ms poll loop (~2.4 ms real period): about 1.2 ms average press-to-latch |
| Read | 4021 latch plus 8-bit shift-out: about 0.4 ms at the default 80 MHz |
| Hand-off | Reports ship on input change (task notify), not on a timer tick: tens of us |
| Air, BLE | Interval pinned at 7.5 ms (the BLE floor on this silicon): 3.75 ms average wait |
| Air, Classic | QoS requests t_poll about 10 ms to pin the link active; typical wait a few ms |

Board-side total, press to radio hand-off, is roughly 6 ms average on BLE and 4 ms on Classic — both
inside a single 16.7 ms frame, and small against the host stack and display downstream. Pinning the
BLE interval at 7.5 ms roughly halves worst-case report wait versus a host-chosen 15 ms. BLE's
retransmit granularity is a whole interval (+7.5 ms) where Classic retries a slot at a time
(+1.25 ms), so Classic has both the better average and the gentler jitter tail.

The Classic figure is the softer of the two: the QoS t_poll pin does not actually guarantee the link
stays out of sniff. A real Switch was observed holding the link in sniff steady state regardless (see
`../history/power-tuning/README.md`), which would put Classic latency under the negotiated sniff
interval rather than t_poll. That interval has not been measured.

### Player select and two-player

The BLE transport registers the same report map twice (two HID services with distinct report IDs),
so hosts enumerate two gamepads and the player-select slider routes the stick to P1's or P2's report
for take-turns play. Classic is one Pro Controller (one player); the slider only picks which. The
host-by-host behavior is in [MANUAL.md](MANUAL.md).

## Protocol reference

The byte-level Switch Pro Controller emulation contract (identity, input/output reports, handshake
order, sniff and connection-direction behavior) is in
[`switch_pro_protocol.md`](switch_pro_protocol.md).
