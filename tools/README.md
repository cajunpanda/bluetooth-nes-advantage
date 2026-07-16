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

## Pre-build Bluedroid patches (removed)

This directory used to hold `patch_bluedroid_*.py`, PlatformIO `extra_scripts` that edited the
Bluedroid Classic stack in place to make its HID device behave like a real Pro Controller. The
Classic transport now runs on BTstack, which does all of it natively, and Bluedroid's Classic half
is compiled out - so the patches are gone. They are in git history if a Bluedroid Classic path is
ever revived. See [`../docs/FIRMWARE.md`](../docs/FIRMWARE.md) "Two Bluetooth host stacks" and
[`../firmware/components/btstack/README.md`](../firmware/components/btstack/README.md).
