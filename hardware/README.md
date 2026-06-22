# Hardware

Open hardware for the Bluetooth NES Advantage.

| Path | Contents |
|---|---|
| [`bt-nes-advantage-pcb/`](bt-nes-advantage-pcb/) | KiCad 10 PCB project (schematic and 2-layer layout) |
| [`bt-nes-advantage-jack-plug/`](bt-nes-advantage-jack-plug/) | 3D-printable DC jack plug (STL and FreeCAD source) |

The full build reference (architecture, BOM, pin and connection maps, power path, assembly, and
layout notes) is in [`../docs/HARDWARE.md`](../docs/HARDWARE.md). Pin assignments match the firmware
(`../firmware/main/board_config.h`).

Board highlights:

- **MCU:** bare ESP32-WROOM-32E-N4 on a single 3.3 V rail (the Advantage's logic runs at 3.3 V, so
  there is no 5 V rail and no level shifting).
- **Power:** barrel jack (+5 V) to TP4056 charger, 1S LiPo, discrete load-share power path, TPS63900
  buck-boost to 3.3 V. NES LATCH/CLK/DATA sit on RTC-capable GPIOs so the ULP polls the shift
  registers in deep sleep.
- **Programming and debug:** TC2030 Tag-Connect (J4), pinned for the TC2030-FTDI-C232HD-DDHSP-0-DTR
  USB-UART cable (DTR/RTS auto-reset). No onboard USB; charging is via the barrel jack only.
- **Status:** R/G/B 0603 LEDs (active-low), visible through the transparent jack plug.
- **Board:** 2 layer, 1.6 mm, 78 x 36 mm, fits the stock case with no cutting.
