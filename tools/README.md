# tools/

Host-side helpers for firmware development.

## `serial_proxy.py`: shared serial monitor and flashing

Only one process can own the serial port at a time, so a person and an automated tool cannot both
open it, and you would normally have to stop the monitor before every flash. This proxy owns the
port and tees everything to a shared logfile, and `flash` coordinates the port handoff so nothing
needs to be stopped by hand. `tail -f` survives flashes; it pauses, then the fresh boot streams in.

```bash
# Start the proxy once; it owns the port and runs until stopped.
tools/serial_proxy.py monitor

# Watch live, any number of readers, in any terminal:
tail -f /tmp/btna_serial.log

# Flash without touching the monitor: pause proxy, pio build + upload, resume + reset.
# On a successful flash the log is truncated for a fresh start:
tools/serial_proxy.py flash
tools/serial_proxy.py flash --no-truncate                   # keep the existing log
tools/serial_proxy.py flash -- --upload-port /dev/ttyUSB1   # extra pio args after `--`

tools/serial_proxy.py reset     # reboot the chip (pulse DTR/RTS)
tools/serial_proxy.py status    # proxy pid / port / log
tools/serial_proxy.py stop      # stop the proxy
tools/serial_proxy.py tail      # follow the log in this terminal
```

It survives power cycles and replugs: the proxy watches the port and auto-reopens when the
USB-serial adapter re-enumerates, flushing the power-on glitch.

How the flash handoff works: `flash` signals the running proxy to release the port, waits, runs
`pio run -e wroom32 -t upload`, then signals resume and reset so a clean boot is always captured. If
no proxy is running, `flash` flashes directly. A paused proxy auto-resumes after 180 s as a safety
net.

The default port is a stable `/dev/serial/by-id/*` symlink when present (it follows the adapter
across `ttyUSBn` renumbering), falling back to `/dev/ttyUSB0`. Override with `--port` or
`SERIAL_PROXY_PORT`. Log and state live in `/tmp` (`/tmp/btna_serial.log`,
`/tmp/btna_serial_proxy.json`); override with `SERIAL_PROXY_LOG` / `SERIAL_PROXY_BAUD`. Requires
`pyserial`.

## `patch_bluedroid_sniff.py`: pre-build Bluedroid patch

Applied automatically by the PlatformIO build (`extra_scripts` in `platformio.ini`). It makes the
Bluedroid Classic HID device present as a link-layer slave that never initiates sniff, matching real
Pro Controller behavior. Required for stable connections to the 8BitDo receiver and the Switch; a
no-op for BLE builds. See [`../docs/switch_pro_protocol.md`](../docs/switch_pro_protocol.md).
