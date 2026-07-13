# tools/

Host-side helpers for firmware development.

## Serial monitor and flashing: benchmux

Firmware bring-up (owning the UART, tailing the boot log, flashing without stopping the monitor)
uses **benchmux**, a standalone bench serial proxy, rather than a script bundled here. Only one
process can own a serial port, so benchmux owns it and tees everything to a shared logfile; any
number of readers follow it live while `flash` coordinates the port handoff, so nothing is stopped
by hand. `tail -f` survives flashes; it pauses, then the fresh boot streams in.

benchmux ships `serial_proxy.py` (USB serial console + flash) and `ble_proxy.py` (BLE console +
OTA). Run it by path, or symlink it onto `PATH`:

```bash
serial_proxy.py monitor --port /dev/ttyUSB0                       # own the port, tee to /tmp/serial_proxy.log
tail -f /tmp/serial_proxy.log                                     # watch live, any number of readers
serial_proxy.py flash --flash-cmd 'pio run -e wroom32 -t upload'  # build + upload, monitor stays up
serial_proxy.py reset                                             # reboot the chip (pulse DTR/RTS)
serial_proxy.py send help                                         # inject a console line
serial_proxy.py status                                            # proxy pid / port / log
serial_proxy.py stop                                              # stop the proxy
```

With a `platformio.ini` present, `flash` defaults to `pio run -t upload`; pass `--flash-cmd` to pin
the env (as above) or drive another toolchain. The proxy auto-reopens when the USB-serial adapter
re-enumerates across `ttyUSBn` renumbering; pin one adapter with `--port` (a `/dev/serial/by-id/*`
substring, e.g. `TC2030`, or an exact path). Log defaults to `/tmp/serial_proxy.log`
(`--log` / `SERIAL_PROXY_LOG`). `pio device monitor` needs a real TTY; reading the logfile is the
scriptable path.

## `patch_bluedroid_sniff.py`: pre-build Bluedroid patch

Applied automatically by the PlatformIO build (`extra_scripts` in `platformio.ini`). It makes the
Bluedroid Classic HID device present as a link-layer slave that never initiates sniff, matching real
Pro Controller behavior. Required for stable connections to the 8BitDo receiver and the Switch; a
no-op for BLE builds. See [`../docs/switch_pro_protocol.md`](../docs/switch_pro_protocol.md).
