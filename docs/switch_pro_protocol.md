# Switch Pro Controller: BT Classic emulation reference

Protocol reference for the Switch Pro Controller emulation in
[`../firmware/main/bt_pro.cpp`](../firmware/main/bt_pro.cpp). It covers the identity, input and
output reports, the handshake, and the two link-layer behaviors the emulation depends on.

## Identification

The Switch does not validate the SDP HID report descriptor. It identifies the controller by
VID/PID `0x057E`/`0x2009`, device name `"Pro Controller"`, and Class of Device, then negotiates
everything over the `0x01` and `0x21` subcommand handshake on the L2CAP interrupt channel. Get the
identity right, answer the subcommands, and stream `0x30`. The descriptor still declares report IDs
0x30/0x21/0x01/0x10 so the ESP-IDF `esp_hidd` layer can route reports by ID.

| Field | Value |
|---|---|
| VID / PID | `0x057E` / `0x2009` |
| Device name | `"Pro Controller"` (exact) |
| Class of Device | major `0x05` (Peripheral), minor gamepad, CoD `0x002508` |
| HID SDP subclass | `0x08` (gamepad) |
| Channels | L2CAP control PSM `0x11`, interrupt PSM `0x13` |

Publish VID/PID via the SDP DI record, set the name and CoD, and be Connectable and Discoverable.

## Input report 0x30 (standard full report)

Byte 0 is the report ID when sent raw. With `esp_hidd_dev_input_set(dev, 0, 0x30, body, len)` the
report ID is passed separately and `body` is bytes 1 to N below (no leading `0x30`, no `0xA1`).

| Byte | Meaning |
|---|---|
| 0 | `0x30` (report ID) |
| 1 | Timer, increment every report, wraps 0x00 to 0xFF |
| 2 | Battery and connection. Use `0x80` (full battery, Pro, not powered) |
| 3 | Buttons, Right group |
| 4 | Buttons, Shared group |
| 5 | Buttons, Left group |
| 6 to 8 | Left stick (12-bit X,Y packed), neutral `00 08 80` |
| 9 to 11 | Right stick, neutral `00 08 80` |
| 12 | Vibrator input (`0x08`) |
| 13 to 48 | 6-axis IMU, Int16LE; all-zero when unused (NES needs none) |

### Button bit map

| Byte | x01 | x02 | x04 | x08 | x10 | x20 | x40 | x80 |
|---|---|---|---|---|---|---|---|---|
| 3 Right | Y | X | B | A | SR | SL | R | ZR |
| 4 Shared | Minus | Plus | R-Stick | L-Stick | Home | Capture | (none) | Chg-Grip |
| 5 Left | Down | Up | Right | Left | SR | SL | L | ZL |

The D-pad is individual bits in byte 5 (not a hat) for report 0x30.

### Stick 12-bit packing

x,y in 0 to 4095, center 2048 (0x800):

```c
b[0] =  x & 0xFF;
b[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
b[2] =  (y >> 4) & 0xFF;            // center 2048 packs to 00 08 80
```

### Neutral idle 0x30 (49 bytes incl. ID, no IMU)

```
30 00 80 00 00 00  00 08 80  00 08 80  08  <36 zero IMU bytes>
```

A 13-byte report with no IMU tail and `report30[12]=0x08` is fine for a non-motion controller.

## Input report 0x21 (subcommand reply)

Same prefix as 0x30 (bytes 0 to 12), then: byte 13 = ACK (`0x80` simple ack, `0x80|datatype` for
typed data such as `0x90` for SPI reads, NACK `0x00`), byte 14 = echoed subcommand id, bytes 15+ =
reply payload (up to 35). The canned prefix is `21 <pkt> 8E 84 00 12 01 18 80 01 18 80 80`.

## Output reports (host to device)

- `0x01`: rumble plus subcommand. `[1]` packet number, `[2..9]` rumble, `[10]` subcommand id,
  `[11..]` args (SPI sub-address at `[11]`/`[12]`). All handshake subcommands arrive here. With
  `esp_hidd` the OUTPUT event strips the report-id byte, so the subcommand id is at `data[9]` and
  the SPI address at `data[10]`/`data[11]`.
- `0x10`: rumble only; ignore.
- `0x80`: UART/handshake, USB-only; BT generally skips it. Ignore, or ack `0x81 <cmd>`.

## Canned 0x21 replies (byte 0 = report id 0x21)

Strip byte 0 and pass `report_id=0x21` to `esp_hidd_dev_input_set`. Prefix
`21 <pkt> 8E 84 00 12 01 18 80 01 18 80 80`, then `<ack> <subcmd> <payload>`.

- `0x02` Device info: ack `0x82`, subcmd `0x02`, firmware `03 48`, controller type `0x03` (Pro),
  then `0x02`, MAC (bytes 19 to 24, stamp the real BD_ADDR), `0x03`, zeros.
- `0x08` Set shipment: ack `0x80 0x08`.
- `0x03` Set input report mode: ack `0x80 0x03`. After this, stream 0x30 continuously.
- `0x04` Trigger elapsed time: ack `0x83 0x04`, then `00 6a 01 bb 01 93 01 95 01 ...`.
- `0x10` SPI flash read: ack `0x90`, subcmd `0x10`, then `addrLE(4) size(1) data[size]`. Dispatch by
  sub-address:
  - `0x603D` factory stick cal, size `0x19`:
    `F0 07 7F  F0 07 7F  F0 07 7F   F0 07 7F  F0 07 7F  F0 07 7F   0F 0F 00 00 00 00`
    (symmetric flat cal; passes the handshake).
  - `0x6080` factory config, size `0x18`:
    `5e 01 00 00 f1 0f  19 d0 4c ae 40 e1  00 00 00 00 00 00  ff ff ff ff ff ff 00 00`
  - `0x6098` stick params 2, size `0x12`:
    `19 d0 4c ae 40 e1  00 00 00 00 00 00  ff ff ff ff ff ff 00 00`
  - `0x6020` factory IMU cal, size `0x18`: zeros.
  - `0x6050` controller colors, size `0x18`: zeros.
  - `0x8010` / `0x8028` user stick/IMU cal: return `0xFF` for the magic so the Switch falls back to
    factory cal.
- `0x21` Set NFC/IR config: ack `0x80 0x21` plus 34 zero bytes. Flip the paired flag here and switch
  from empty reports to full 0x30 streaming.
- `0x30` Set player lights: ack `0x80 0x30`.
- `0x40` Enable IMU: ack `0x80 0x40`.
- `0x48` Enable vibration: ack `0x80 0x48`.
- `0x01` BT manual pairing: not part of the wireless Change-Grip flow; ack `0x80 0x01` if seen.
- Any unhandled subcommand: always answer something (ack `0x80`, echo id) or the Switch stalls.

## Handshake order and timing

1. Until paired, stream a tiny empty report (`00 <timer>`) about every 100 ms to keep the link
   alive.
2. The Switch issues, over output 0x01, roughly: `0x02` info, `0x08` shipment, `0x10` SPI reads
   (stick cal), `0x03` set mode 0x30, `0x04` elapsed, `0x40` IMU, `0x48` vibration, `0x30` lights,
   `0x21` NFC/IR. Answer each within a few ms (synchronously in the data event).
3. After `0x03` to `0x30` is acked, stream full 0x30 at about 60 Hz continuously or the controller
   is dropped. NSO NES needs no IMU or rumble, but you must still ack 0x40/0x48 during the
   handshake.
4. Increment the timer byte every report.

## Link-layer power: never slave-initiate sniff

A real Pro Controller is a BT slave that never initiates sniff. It lets the master (Switch, 8BitDo,
PC) drive power management and only accepts the master's sniff. Stock Bluedroid's HID-device power
spec does the opposite: it slave-initiates sniff about 5 s after connect, on idle, and on "busy"
(the last fires on every report of a 60 Hz stream). The Switch tolerates this, but the 8BitDo NES
receiver refuses it (HCI status `0x24`, "LMP PDU not allowed") and the retry storm tears the link
down and re-pairs repeatedly, so the receiver never holds a stable connection.

Fix: `tools/patch_bluedroid_sniff.py`, a PlatformIO pre-build patch. In the HID-device (HD) entry of
`bta_dm_pm_spec[]` (`components/.../bta/dm/bta_dm_cfg.c`), set all three sniff initiations (conn-open,
idle, busy) to `BTA_DM_PM_NO_ACTION`, leaving the allow-mask (`BTA_DM_PM_SNIFF | BTA_DM_PM_PARK`)
intact so the device still accepts master sniff. The spec is a `const` table with no runtime API or
Kconfig, so it must be patched at build time. With this, 8BitDo holds a stable connection and the
Switch is unaffected. `esp_bt_gap_set_qos()` alone does not fix it; it only momentarily wakes the
link before BTA re-sniffs.

## Connection direction: stay passive, let the host initiate

After authentication, the host opens the two HID L2CAP channels (control PSM `0x11`, interrupt
`0x13`). Every target host initiates this itself: the Switch Change Grip/Order screen, the 8BitDo
Retro Receiver, and the 8BitDo USB Wireless Adapter all page the device and open the channels. So
the device stays purely passive: connectable and discoverable, accepting the incoming connection. Do
not call `esp_bt_hid_device_connect()`.

`esp_bt_hid_device_connect()` puts Bluedroid's HID-device state machine into an initiator state
(`HID_ERR_CONN_IN_PROCESS`). When the host then sends its own incoming connection it lands on
generic L2CAP instead of the HID layer, the config handshake never promotes to a HID connection, and
the link is torn down after a config timeout. `bt_pro.cpp` therefore never device-initiates; even on
a reconnect boot with a stored bond it only seeds the peer address (for QoS) and waits. The
tradeoff is that a Switch console will not auto-reconnect on its own; re-pair from Change Grip/Order.

## ESP-IDF / Bluedroid notes

- Send all input reports on the interrupt channel. Use
  `esp_hidd_dev_input_set(dev, map_index, report_id, body, len)` with `report_id` `0x30`/`0x21`; do
  not also prepend `0xA1` or the report-id byte, the stack frames it.
- Classic-only: release BLE memory (`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`) and enable
  `ESP_BT_MODE_CLASSIC_BT` to free RAM and avoid coexistence issues.
- Stamp the device's real BD_ADDR into the 0x02 device-info reply MAC field.
- Do not go silent between handshake packets; answer subcommands within a few ms.

## Sources

- dekuNukem/Nintendo_Switch_Reverse_Engineering: `bluetooth_hid_notes.md`,
  `bluetooth_hid_subcommands_notes.md`, `spi_flash_notes.md`
- NathanReeves/BlueCubeMod: `Firmware/BlueCubeModv2/main/main.c`
- ESP-IDF Bluetooth HID Device API (`esp_hidd`)
