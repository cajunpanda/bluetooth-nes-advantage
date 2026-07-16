# Switch Pro Controller: BT Classic emulation reference

Protocol reference for the Switch Pro Controller emulation in
[`../firmware/main/bt_pro_btstack.cpp`](../firmware/main/bt_pro_btstack.cpp) (BTstack; see
[FIRMWARE.md](FIRMWARE.md) "Two Bluetooth host stacks"). It covers the identity, input and
output reports, the handshake, and the two link-layer behaviors the emulation depends on.

## Identification

The Switch does not validate the SDP HID report descriptor. It identifies the controller by
VID/PID `0x057E`/`0x2009`, device name `"Pro Controller"`, and Class of Device, then negotiates
everything over the `0x01` and `0x21` subcommand handshake on the L2CAP interrupt channel. Get the
identity right, answer the subcommands, and stream `0x30`. The descriptor still declares report IDs
0x30/0x21/0x01/0x10 so the local HID layer can route reports by ID.

| Field | Value |
|---|---|
| VID / PID | `0x057E` / `0x2009` |
| Device name | `"Pro Controller"` (exact) |
| Class of Device | major `0x05` (Peripheral), minor gamepad, CoD `0x002508` |
| HID SDP subclass | `0x08` (gamepad) |
| Channels | L2CAP control PSM `0x11`, interrupt PSM `0x13` |

Publish VID/PID via the SDP DI record, set the name and CoD, and be Connectable and Discoverable.

## Input report 0x30 (standard full report)

Byte 0 is the report ID when sent raw. On the wire an interrupt-channel input report is
`A1 30 <body>`: the HID DATA|INPUT header `0xA1`, the report ID, then bytes 1 to N below.
BTstack's `hid_device_send_interrupt_message()` sends the buffer verbatim, so the caller supplies
that whole frame itself (`bt_pro_btstack.cpp send_report()`).

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
  `[11..]` args (SPI sub-address at `[11]`/`[12]`). All handshake subcommands arrive here. Both
  BTstack's report-data callback and Bluedroid's OUTPUT event hand over the payload with the
  report-id byte already stripped, so the subcommand id is at `data[9]` and the SPI address at
  `data[10]`/`data[11]`. **BTstack gotcha:** it drops output reports whose length does not match
  the descriptor (`hid_report_size_valid()`), and the console's `0x01` frames are shorter than the
  48 bytes we declare - `hid_device_accept_truncated_hid_reports(true)` is required or every
  subcommand is silently discarded.
- `0x10`: rumble only; ignore.
- `0x80`: UART/handshake, USB-only; BT generally skips it. Ignore, or ack `0x81 <cmd>`.

## Canned 0x21 replies (byte 0 = report id 0x21)

Sent as `A1 21 <body>` on the interrupt channel. Prefix
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

On BTstack this is free: it never initiates sniff unless the application asks. We allow the policy
(`gap_set_default_link_policy_settings(... | LM_LINK_POLICY_ENABLE_SNIFF_MODE)`) so a master's
sniff is still accepted, and set the SDP record's SSR fields to `0xFFFF` so we request no sniff
subrating either.

(Historical: under Bluedroid this needed a build-time patch, `tools/patch_bluedroid_sniff.py`, to
null all three sniff initiations in the `const` `bta_dm_pm_spec[]` HD entry - there was no runtime
API or Kconfig for it. `esp_bt_gap_set_qos()` alone did not fix it; it only momentarily woke the
link before BTA re-sniffed. See git history before the BTstack switch.)

## Connection direction: passive first, then a bounded device-initiated "kick"

After authentication, the two HID L2CAP channels (control PSM `0x11`, interrupt `0x13`) must open.
Hosts differ in who they expect to open them:

- The Switch Change Grip/Order screen and the 8BitDo Retro Receiver open both channels themselves,
  promptly after auth.
- The 8BitDo USB Adapter 2 and BlueRetro mimic the console's *reconnect* role: they expect the
  controller to behave like a real Pro (which advertises `HIDReconnectInitiate` and opens the
  channels itself). If the device stays passive they wait forever and give up after ~30 s.

So `bt_pro_btstack.cpp` starts passive (host-initiated connects just work: BTstack's `hid_device`
auto-accepts incoming CTRL/INTR) and arms a one-shot "kick" timer on pairing complete, on a bonded
boot, and on disconnect: if the host has not opened HID within a 1.5 s grace window, the device
calls `hid_device_connect()` itself, with bounded retries (4 x 5 s) before falling back to passive
so a dead host cannot pin the radio awake. A successful `HID_SUBEVENT_CONNECTION_OPENED` cancels
the kick. The grace window also avoids racing a host that is opening its own connection.

Grace-window sizing: it must be long enough that hosts which open both channels themselves (Switch
console, 8BitDo Retro Receiver - both well under 1 s) always win, but short enough to beat
BlueRetro, which opens CTRL and waits only ~2 s for the controller's INTR before tearing down and
retrying. A 3 s grace loses that race and the two sides then collide (orphan L2CAP channel,
subcommand replies lost, link dropped after ~30-60 s, repeat).

BlueRetro is a third pattern: it does open both channels itself, but only ~3 s after auth (it runs
an SDP query first), and it opens CTRL within milliseconds of the SSP link key - before encryption
has settled.

### Historical: what this cost under Bluedroid

Bluedroid needed five build-time source patches (`tools/patch_bluedroid_hid_intr.py`, since
deleted - see git history before the BTstack switch) to survive the above against the 8BitDo USB
Adapter 2 and BlueRetro. They are recorded here because they document real ESP32/host quirks, and
because a future BTstack bug may rhyme with one:

1. **MITM link-key "upgrade" wedged the ESP32 controller.** Bluedroid's HID service registered with
   `AUTHENTICATE|ENCRYPT`, and in SSP mode btm silently added a MITM requirement. We pair Just
   Works (unauthenticated key), so when a host advertised IO caps making an authenticated key
   possible (the Adapter 2 does; a console reports NoInputNoOutput and does not),
   `btm_sec_check_upgrade` deleted the fresh key and re-authenticated the already-encrypted link.
   The ESP32 controller's LMP encryption pause/resume did not survive it: ACL TX froze (no
   Number-of-Completed-Packets, no Encryption Key Refresh Complete; occasionally an
   `ASSERT_WARN lc_task.c 1556` controller crash) and both sides deadlocked for 30 s.
   **Still relevant:** a real Pro uses Just-Works keys with no MITM, and this controller dislikes
   re-authenticating a live encrypted link. BTstack does not do this by default.
2. **Half-open connections were never completed** - stock Bluedroid only self-originated INTR when
   it had originated CTRL, deadlocking with hosts that open CTRL and wait. BTstack's
   `hid_device_connect()` opens CTRL then INTR itself.
3. **The btm channel-security gate re-litigated security per HID L2CAP connect**, refusing a CTRL
   connect that raced SSP pairing. BTstack has no equivalent gate.
4. **Abandoned originate attempts left zombie channels** that the peer reaped 16-30 s later,
   taking the session with it.
5. **MTU**: `HID_DEV_MTU_SIZE` had to be raised from 64 to 640 to match a real Pro's L2CAP config.

## Input report modes: 0x3F by default, 0x30 on request

A real Pro Controller powers up in simple input mode - report `0x3F` (2 button bytes, hat, four
16-bit axes), sent on state change - and only streams full `0x30` reports after the host selects
that mode with subcommand `0x03`. The Switch console and the 8BitDo adapters send `0x03 0x30`
during their handshake; BlueRetro never sends `0x03` and reads `0x3F` reports. The firmware
therefore starts every connection in `0x3F` mode (sent on change plus a ~100 ms keepalive) and
switches to the continuous ~66 Hz `0x30` stream when subcommand `0x03` asks for it.

## ESP32 / BTstack notes

- Send all input reports on the interrupt channel, as a self-framed `A1 <report_id> <body>` buffer
  passed to `hid_device_send_interrupt_message()`. Sending is gated: call
  `hid_device_request_can_send_now_event()` and build the frame in the
  `HID_SUBEVENT_CAN_SEND_NOW` handler.
- `hid_device_accept_truncated_hid_reports(true)`, or the console's short `0x01` output reports are
  silently dropped and no subcommand ever arrives.
- Classic-only: release BLE memory (`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`) and enable
  the controller in `ESP_BT_MODE_CLASSIC_BT` to free RAM and avoid coexistence issues.
- BTstack is single-threaded: call its API only from the run loop, and bring the stack up **on the
  run-loop task** - `btstack_run_loop_init()` records its caller as the task to notify for all
  cross-thread work, including the VHCI transport's own packet delivery. Initialize it from the
  wrong task and the stack silently never reaches `HCI_STATE_WORKING`. Hand work in from other
  tasks with `btstack_run_loop_execute_on_main_thread()`.
- Stamp the device's real BD_ADDR (`gap_local_bd_addr()`) into the 0x02 device-info reply MAC field.
- Do not go silent between handshake packets; answer subcommands within a few ms.
- Debugging: `hci_dump_init(hci_dump_embedded_stdout_get_instance())` puts a full HCI trace on the
  serial console (`BTNA_HCI_DUMP` in `bt_pro_btstack.cpp`). This is the only way to see the Switch 2
  handshake.

## Sources

- dekuNukem/Nintendo_Switch_Reverse_Engineering: `bluetooth_hid_notes.md`,
  `bluetooth_hid_subcommands_notes.md`, `spi_flash_notes.md`
- NathanReeves/BlueCubeMod: `Firmware/BlueCubeModv2/main/main.c`
- BTstack (`bluekitchen/btstack`): `src/classic/hid_device.h`, `example/hid_keyboard_demo.c`
- Other BTstack-based Pro Controller emulators: `DavidPagels/retro-pico-switch`,
  `Wilstride/PicoSwitchController` (both Pico W; same GAP/SDP parameter choices)
