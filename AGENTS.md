# Agent notes

Orientation for AI coding agents and new contributors working in this repo.

## What this is

Custom hardware and firmware that turns an NES Advantage joystick into a Bluetooth controller. Three
coupled artifacts:

- `firmware/`: ESP-IDF v5 (C++) for a bare ESP32-WROOM-32E, built with PlatformIO
- `hardware/`: KiCad 10 PCB project plus the 3D-printable jack plug
- `web/`: dependency-free Web Bluetooth page for settings and OTA updates

## Sources of truth

- Pin map: `firmware/main/board_config.h` and `docs/HARDWARE.md` describe the same pins and must stay
  in sync. If you change one, change the other.
- Hardware (BOM, connection maps, layout): `docs/HARDWARE.md`
- Firmware build, architecture, sleep/wake, transports, config mode, latency: `docs/FIRMWARE.md`
- Switch Pro protocol bytes: `docs/switch_pro_protocol.md`
- The GATT contract is duplicated by design in `firmware/main/bt_config.cpp` and `web/index.html` (UUIDs,
  opcodes, flow control). Change them together.

## Build and test

Firmware (from `firmware/`):

```bash
pio run -e wroom32             # build the gameplay firmware
```

A build is the main static check; there is no unit-test suite. Treat warnings in `main/*.cpp` as
regressions.

Flashing and serial, when hardware is attached, go through **benchmux** (a standalone bench serial
proxy, `serial_proxy.py`) so multiple readers (a human terminal and an agent reading the log file)
can watch one port. Run it by path or symlinked onto `PATH`:

```bash
serial_proxy.py monitor --port /dev/ttyUSB0                       # own the port, tee to /tmp/serial_proxy.log
serial_proxy.py flash --flash-cmd 'pio run -e wroom32 -t upload'  # build + upload without stopping the monitor
tail -f /tmp/serial_proxy.log                                     # or read the file directly
```

Do not open the serial port directly while the proxy runs. `pio device monitor` needs a real TTY;
reading the proxy's log file is the agent-friendly path. See [`tools/README.md`](tools/README.md).

PCB checks (KiCad 10 CLI; on some machines KiCad is a flatpak, adjust to your install):

```bash
kicad-cli sch erc --severity-all hardware/bt-nes-advantage-pcb/bt-nes-advantage-pcb.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity hardware/bt-nes-advantage-pcb/bt-nes-advantage-pcb.kicad_pcb
# flatpak form: flatpak run --command=kicad-cli org.kicad.KiCad <args>
```

Both are expected clean. Known accepted DRC noise: two excluded mounting-hole keepout markers,
silkscreen overlaps at the LEDs, and hand-solder pad library mismatches. Flatpak sandboxes `/tmp`, so
write CLI outputs somewhere under the repo or `$HOME`.

## Hardware constraints that bite

- The production board has no accessible reset or power button once installed. Anything that can wedge
  the device must have a firmware-driven escape (timeouts, watchdogs, gestures). Never remove the
  existing escapes from config/OTA mode.
- NES LATCH/CLK/DATA must stay on RTC-capable GPIOs (the ULP polls them in deep sleep), battery sense
  must stay on ADC1, and GPIO12 must never be used.
- The NES DATA lines need opposite pull directions awake (pull-down) vs asleep (pull-up). They use
  the ESP32 internal pulls; R14/R15 on the board stay unpopulated.
- Tests that need hands on the physical controller (button gestures, pairing against a console)
  require the user. Say what you want pressed and let them drive.

## Conventions

- Source files carry SPDX headers; licensing is per-medium (see `LICENSING.md`): MIT for firmware,
  web, and tools; CERN-OHL-P-2.0 for hardware; CC-BY-4.0 for docs.
- `references/` (local datasheets) is gitignored and not part of the public repo; do not link to it.
- Comments and docs: terse, present-tense, describe the current state only. No change history, no
  "Rev/Option/Stage" references, no dated war stories. No em or en dashes, no emoji, no arrow chains.
  Plain hyphens and ordinary punctuation. Write like an engineer, not a changelog.
