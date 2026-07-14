# web: BLE configuration and OTA web app

A single-page Web Bluetooth app to test the controller, configure it, and flash new firmware
over the air. No build step, no dependencies, no external requests: one self-contained
`index.html` (inline CSS/JS). The look is a grey-and-red front-loader NES rendered as UI; see
`DESIGN.md` and `PRODUCT.md`.

## Use

1. On the controller, hold **A+B+Select for 5 s** to reboot into config/OTA mode. The
   device advertises as **"NES Advantage Config"**; the blue LED blinks while waiting and
   goes solid when connected. (On the bench you can instead enter config mode over serial
   with the `config` console command.)
2. Serve this folder from a secure context. `http://localhost` is exempt, otherwise HTTPS:
   ```bash
   cd web && python3 -m http.server 8770
   ```
   Open <http://localhost:8770/> in Chrome or Edge.
3. **Connect** and pick "NES Advantage Config". The first GATT connect on Chrome/Linux
   sometimes throws a transient `NetworkError`; the page auto-retries (a known BlueZ quirk)
   and silently reconnects if the link drops.
4. **Test:** watch every button, the D-pad, the P1/P2 select switch, and the turbo dials
   light up live, alongside a battery meter. **Configure:** change transport / button profile
   / directional mode and **Apply & reboot**. **System:** drop
   a `.bin` on the cartridge slot to flash firmware over the air, and use the device console
   (live log + the same commands as the wired UART console).

To leave config mode without a browser (the production board has no reset button): hold
**Start for about 3 s** on the controller, or wait; it returns to gameplay after 5 idle
minutes.

## Browser support

Web Bluetooth requires Chrome, Edge, or another Chromium browser. On Linux it talks to
BlueZ; if `navigator.bluetooth` is missing, enable
`chrome://flags/#enable-experimental-web-platform-features`. Safari has no Web Bluetooth; on
iPhone use a Web BLE browser such as Bluefy or BLE-Link. Firefox is unsupported.

## GATT contract

Service `5f1d0000-7c5a-4e2a-9b6e-2a8f3c9d1e00` with seven characteristics:

| UUID suffix | Name | Properties | Payload |
|---|---|---|---|
| `0001` | INFO | read + notify | JSON device info + settings |
| `0002` | CMD | write | JSON `{transport\|profile\|dirmode}` or `{action:"reboot\|forget"}` |
| `0003` | OTACTL | write + notify | OTA control opcodes / status codes |
| `0004` | OTADATA | write-no-resp | raw firmware chunks |
| `0005` | INPUT | read + notify | live controller frame (7 bytes: player, buttons, turbo A/B Hz, battery) |
| `0006` | LOG | notify | device console / log stream (UTF-8) |
| `0007` | CONSOLE | write | console command line (UTF-8); output streams back on LOG |

The firmware side is `../firmware/main/bt_config.cpp`; the two files are kept in lockstep
(UUIDs, OTA opcodes, the windowed/ACK flow control, and the INPUT frame layout). A source
build's image for OTA is `firmware/.pio/build/wroom32/firmware.bin`.
</content>
