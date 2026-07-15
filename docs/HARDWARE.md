# Hardware

How to build the Bluetooth NES Advantage board. You need the PCB, the components below, and a
soldering iron. Everything is hand-solderable, though the two QFN-style regulators (U2, U3) are
easiest with hot air or a hotplate. To fit a finished board into a controller, see
[INSTALL.md](INSTALL.md).

Pin assignments here match the firmware (`firmware/main/board_config.h`). The KiCad project is in
[`../hardware/`](../hardware/) and the schematic is ERC-clean.

## PCB

- Order it ready-made from the
  [PCBWay shared project](https://www.pcbway.com/project/shareproject/Bluetooth_NES_Advantage_865d24ef.html)
  (gerbers preloaded, one-click ordering).
- Or fab it yourself from the KiCad project in
  [`../hardware/bt-nes-advantage-pcb/`](../hardware/bt-nes-advantage-pcb/): standard 2-layer,
  1.6 mm, 78 x 36 mm. It fits the stock case with no cutting.

## Bill of materials

39 placed parts; R14/R15 are do-not-populate (footprint only). "HS" footprints are hand-solder
variants.

| Ref(s) | Qty | Value / MPN | Footprint | Notes |
|---|---|---|---|---|
| U1 | 1 | ESP32-WROOM-32E-N4 | RF_Module:ESP32-WROOM-32E | MCU module, 4 MB |
| U2 | 1 | TP4056-42-ESOP8 | SOIC-8-1EP (EP 2.29x3 mm) | 1S Li-ion linear charger |
| U3 | 1 | TPS63900DSKR | WSON-10-1EP (2.5x2.5 mm) | Buck-boost, 75 nA Iq |
| Q1 | 1 | AO3401A P-MOSFET | SOT-23 | Power-path battery pass |
| Q2 | 1 | 2N7002LT1G N-MOSFET | SOT-23 | Battery-sense divider ground switch |
| D4 | 1 | LTST-C193KRKT-5A (Red) | LED_0603 HS | Status LED (GPIO19) |
| D5 | 1 | LTST-C193KGKT-5A (Green) | LED_0603 HS | Status LED (GPIO21) |
| D6 | 1 | LTST-C193TBKT-5A (Blue) | LED_0603 HS | Status LED (GPIO22) |
| D7 | 1 | SS14 (Schottky, 40 V / 1 A) | Diode_SMD:D_SMA | Power-path input OR-diode |
| L1 | 1 | 2.2 uH, Isat >= 2 A, low DCR (DFE201610P-2R2M) | Inductor_SMD:L_Murata_DFE201610P | TPS63900 inductor |
| C1,C4,C5,C6,C7,C9 | 6 | 22 uF 10 V X5R (GRM21BR61A226ME44L) | C_0805 HS | IC bulk and controller decoupling |
| C2,C3,C8,C10 | 4 | 100 nF X7R (CL10B104KB8NNNC) | C_0603 HS | HF decoupling, EN delay, ADC filter |
| R1,R2,R3 | 3 | 200 ohm 5% (RC0603JR-07200RL) | R_0603 HS | LED current limit |
| R4,R5,R7,R8,R9 | 5 | 10 kohm 5% (RC0603JR-0710KL) | R_0603 HS | Pull-ups and power-path gate-up |
| R6 | 1 | 1.5 kohm 5% (RC0603JR-071K5L) | R_0603 HS | TP4056 PROG, Ichg about 780 mA |
| R10,R12,R13 | 3 | 1 Mohm 5% (RC0603JR-071ML) | R_0603 HS | Power-path gate-down and battery divider |
| R11 | 1 | 16.2 kohm 1% (RC0603FR-0716K2L) | R_0603 HS | TPS63900 CFG3, sets 3.3 V (needs +/-3% total) |
| R14,R15 | 2 | DNP | R_0603 HS | DATA pulls done by ESP32 internal pulls (see Connection maps) |
| J1 | 1 | CUI PJ-040-SMT-TR | project_footprints:CUI_PJ-040-SMT-TR | Barrel jack, +5 V charge input |
| J2 | 1 | JST S8B-XH-A(LF)(SN) | JST_XH_S8B-XH-A 1x08 | NES Advantage interface |
| J3 | 1 | JST S2B-PH-K-S(LF)(SN) | JST_PH_S2B-PH-K 1x02 | 1S LiPo battery |
| J4 | 1 | Tag-Connect TC2030-IDC | Tag-Connect_TC2030-IDC-FP 2x03 | UART program/debug, needs no part |

## Accessories and tools

- **Battery:** [103450 3.7 V LiPo](https://a.co/d/7UFYwX9), 1500 to 2000 mAh with a JST-PH plug.
  Check polarity before connecting; there is no standard.
- **J2 wiring harness:** [8-pin JST-XH harness cable](https://a.co/d/abYfjqw).
- **Charging cable:** [5 V DC barrel jack cable (2.5 x 0.7 mm)](https://a.co/d/4bPUQj2).
- **Programming cable (self-built boards only):** Tag-Connect
  [TC2030-FTDI-C232HD-DDHSP-0-DTR](https://www.tag-connect.com/product/tc2030-ftdi-c232hd-ddhsp-0-dtr)
  (USB to TC2030 serial, 3.3 V, with DTR for auto-reset). The legged version clips into the board;
  the no-legs version works with the
  [TC2030-CLIP](https://www.tag-connect.com/product/tc2030-retaining-clip-board-3-pack). Only
  needed to put the first firmware on a blank board. Kit boards come pre-flashed, and all updates
  after that happen over the air from the browser.

## Build the board

1. Solder the SMD parts: resistors and capacitors first, then D7 and the LEDs (watch polarity), then
   U2 and U3 (exposed pads, hot air or hotplate recommended), then L1, Q1, Q2, and the ESP32 module.
   Finish with J1, J2, and J3.
2. Flash the firmware before installing, so you can verify the board on the bench. See
   [FIRMWARE.md](FIRMWARE.md) for the first flash over the Tag-Connect cable.

Once the board is built and flashed, install it into the controller: see [INSTALL.md](INSTALL.md).

## Connection maps

### ESP32-WROOM-32E (U1)

| GPIO | Net | Connects to | Notes |
|---|---|---|---|
| GPIO25 | LATCH | J2 (4021 latch) | RTC out (ULP drives in sleep) |
| GPIO26 | CLOCK_0 | J2 (CLK P1) | RTC out |
| GPIO27 | CLOCK_1 | J2 (CLK P2) | RTC out |
| GPIO32 | DATA_0 | J2 (data P1); R14 DNP | RTC in, ESP32 internal pull |
| GPIO33 | DATA_1 | J2 (data P2); R15 DNP | RTC in, ESP32 internal pull |
| GPIO34 | BATT_SENSE | R12/R13 divider mid; C8 filter | ADC1_CH6, input-only |
| GPIO13 | BATT_EN | Q2 gate | enables divider only while sampling |
| GPIO19 | LED-R | +3.3V to R1 to D4 to GPIO19 | active-low |
| GPIO21 | LED-G | +3.3V to R2 to D5 to GPIO21 | active-low |
| GPIO22 | LED-B | +3.3V to R3 to D6 to GPIO22 | active-low |
| GPIO23 | CHG_STAT | TP4056 CHRG; R7 pull-up | open-drain status in |
| GPIO18 | STBY_STAT | TP4056 STDBY; R8 pull-up | open-drain status in |
| GPIO1 (U0TXD) | UART_TX | J4 pin 3 | to cable RXD |
| GPIO3 (U0RXD) | UART_RX | J4 pin 4 | from cable TXD |
| EN | EN | J4 pin 6; R4 pull-up + C3 | reset; from cable RTS |
| GPIO0 | BOOT | J4 pin 2; R5 pull-up | boot/download strap; from cable DTR |
| 3V3 | +3.3V | U3 VOUT; C1/C2 at module | |
| GND | GND | plane; incl. module thermal pad | |

Module power decoupling: C1 (22 uF) + C2 (100 nF) at the 3V3 pin. GPIO12 (MTDI strap) must never be
driven. The DATA lines use the ESP32 internal pulls (opposite polarity awake vs asleep), so R14/R15
stay DNP.

### TP4056 charger (U2)

| Pin | Name | Net / connection |
|---|---|---|
| 1 | TEMP | GND (thermistor sensing disabled) |
| 2 | PROG | R6 (1.5 kohm) to GND, Ichg about 780 mA |
| 3 | GND | GND (+ EPAD) |
| 4 | VCC (VIN) | +5V; C4 (22 uF) decouple. VIN range 4 to 8 V |
| 5 | BAT | +BATT; C5 (22 uF) decouple |
| 6 | STDBY | STBY_STAT to U1 GPIO18; R8 pull-up to +3.3V |
| 7 | CHRG | CHG_STAT to U1 GPIO23; R7 pull-up to +3.3V |
| 8 | CE | +5V (charger enable) |
| EP | EPAD | GND |

### TPS63900 regulator (U3)

| Pin | Name | Net / connection |
|---|---|---|
| 1 | EN | Vsys (always-on; rail must stay up for the ULP) |
| 2 | SEL | GND (selects VO(1), the fixed setpoint) |
| 3 | CFG1 | GND |
| 4 | CFG2 | GND (unlimited input-current limit) |
| 5 | CFG3 | R11 (16.2 kohm 1%) to GND, sets 3.3 V |
| 6 | VOUT | +3.3V; C7 (22 uF) |
| 7 | LX2 | L1 |
| 8 | GND | GND |
| 9 | LX1 | L1 |
| 10 | VIN | Vsys; C6 (22 uF) |
| 11 | GND (EP) | GND |

### Connectors

- **J1 barrel jack:** +5V, GND. Aligned to the Advantage's existing cable hole.
- **J2 NES Advantage (8-pin JST-XH):** 1 DATA_0, 2 LATCH, 3 CLOCK_0, 4 DATA_1, 5 CLOCK_1, 6 LATCH,
  7 +3.3V, 8 GND. Pins 1 to 3 are the P1 4021 bundle, 4 to 6 the P2 bundle; the shared latch line
  gets one conductor per shift register (pins 2 and 6 are the same net). Decouple +3.3V/GND right
  here with C9 (22 uF) + C10 (100 nF).
- **J3 battery (2-pin JST-PH):** +BATT, GND.
- **J4 TC2030 (6-pin):** 1 +3.3V, 2 BOOT (GPIO0), 3 UART_TX (GPIO1), 4 UART_RX (GPIO3), 5 GND,
  6 EN. Matches the TC2030-FTDI-C232HD-DDHSP-0-DTR cable pin-for-pin (1 VCC, 2 DTR, 3 RXD, 4 TXD,
  5 GND, 6 RTS): DTR drives BOOT and RTS drives EN for esptool auto-reset, and the cable's
  regulated 3.3 V / 250 mA VCC shares the rail (or the board runs on battery). The legged footprint
  also takes the no-legs cable plus a TC2030-CLIP.

### Power nets

- `+5V`: J1 to TP4056 VCC/CE (C4) and power-path input (D7 anode).
- `+BATT`: cell (J3) to TP4056 BAT (C5), power-path Q1 drain, and battery divider (R12).
- `Vsys`: power-path output to TPS63900 VIN/EN (C6).
- `+3.3V`: TPS63900 VOUT (C7) to U1, J2, pull-ups, LEDs.

## Layout notes

- **Antenna keepout:** no copper or parts under or beside the WROOM antenna; point it off the board
  edge.
- **Module decoupling:** C1 + C2 right at U1's 3V3 pin; short wide trace; stitch the module pad to
  GND with vias.
- **Controller decoupling:** C9 + C10 at J2, with low-impedance +3.3V/GND pours. Do not skip this.
- **Buck-boost:** tight C6-L1-C7-PGND loop, small switch node, CFG resistors close to the pins;
  keep R11 at 1% / 200 ppm.
- **Charger thermal:** TP4056 pad to generous GND copper (about 1 W at high charge); keep it away
  from the cell and antenna.
- **ADC:** keep R12/R13/C8 and the GPIO34 trace short and away from the switch node; ADC1 only.
- **RTC pins:** LATCH/CLK/DATA (25/26/27/32/33) bit-bang the 4021 from the ULP in deep sleep, so
  route them cleanly with no series parts in the path.
- **Test points:** +BATT, Vsys, +3.3V, GND for bring-up and standby-current measurement.
