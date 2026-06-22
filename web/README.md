# web: BLE configuration and OTA web app

A single-page Web Bluetooth app to configure the controller and flash new firmware over
the air. No build step, no dependencies: plain HTML/JS (`index.html` + `app.js`).

## Use

1. On the controller, hold **A+B+Select for 5 s** to reboot into config/OTA mode. The
   device advertises as **"NES Advantage Config"**; the blue LED blinks while waiting and
   goes solid when connected.
2. Serve this folder from a secure context. `http://localhost` is exempt, otherwise HTTPS:
   ```bash
   cd web && python3 -m http.server 8770
   ```
   Open <http://localhost:8770/> in Chrome or Edge.
3. **Connect** and pick "NES Advantage Config". The first GATT connect on Chrome/Linux
   sometimes throws a transient `NetworkError`; the page auto-retries a few times (a known
   BlueZ quirk).
4. Change **transport / button profile / directional mode** and **Apply & reboot**, or
   **forget the host** to re-pair. To update firmware, drop a `.bin` on the OTA card (or
   use the file picker) and **Flash firmware**. Progress streams live; the device verifies
   the image, swaps the active OTA slot, and reboots into the new firmware.

To leave config mode without a browser (the production board has no reset button): hold
**Start for about 3 s** on the controller, or wait; it returns to gameplay after 5 idle
minutes.

## Browser support

Web Bluetooth requires Chrome, Edge, or another Chromium browser. On Linux it talks to
BlueZ; if `navigator.bluetooth` is missing, enable
`chrome://flags/#enable-experimental-web-platform-features`. Firefox and Safari do not
support Web Bluetooth.

## GATT contract

Service `5f1d0000-7c5a-4e2a-9b6e-2a8f3c9d1e00` with four characteristics:
device-info/settings (read + notify, JSON), config-command (write, JSON), OTA-control
(write + notify), and OTA-data (write without response). The firmware side is
`../firmware/main/bt_config.cpp`; the two files are kept in lockstep (UUIDs, OTA opcodes,
and the windowed/ACK flow control). A source build's image for OTA is
`firmware/.pio/build/wroom32/firmware.bin`.
