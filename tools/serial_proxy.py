#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins
"""Serial proxy and flasher: one port owner, many readers, no manual juggling.

Only one process can open /dev/ttyUSB0, so a human and an automated tool (IDE task,
CI step, AI agent) can't both watch it, and you normally have to stop the monitor
before flashing. This proxy fixes both:

  * It OWNS the port and tees everything to a shared logfile, so any number of
    readers follow it live (`tail -f /tmp/btna_serial.log`, file reads, etc.).
  * `flash` is a first-class subcommand: it tells the running proxy to briefly
    release the port, runs `pio ... upload`, and the proxy auto-reattaches and
    resumes logging. You never kill/restart anything by hand; `tail -f` survives it
    (it just pauses during the flash, then the fresh boot log streams in).
  * It survives power-cycles / replug: `poll()` detects the USB-serial chip
    re-enumerating and auto-reopens the port (flushing the power-on glitch), so a
    power cycle no longer leaves the line stuck spewing garbage until a manual reset.

Usage:
  tools/serial_proxy.py monitor [--port P] [--baud B] [--log PATH] [--no-reset]
        Own the port and tee to the log (run in a terminal or as a background
        process). Then, anywhere:
            tail -f /tmp/btna_serial.log
  tools/serial_proxy.py flash [--env wroom32] [--no-truncate] [-- EXTRA pio args...]
        Pause the proxy, build+upload, resume + reset. On success the log is truncated for a
        fresh start (keeps context small; --no-truncate keeps it). Direct flash if no proxy.
  tools/serial_proxy.py reset      # pulse DTR/RTS to reboot the chip
  tools/serial_proxy.py status     # show proxy pid / port / log
  tools/serial_proxy.py stop       # stop the proxy
  tools/serial_proxy.py tail       # follow the log in this terminal
"""
import argparse
import json
import os
import select
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(os.path.dirname(HERE), "firmware")
STATE = os.environ.get("SERIAL_PROXY_STATE", "/tmp/btna_serial_proxy.json")
DEFAULT_LOG = os.environ.get("SERIAL_PROXY_LOG", "/tmp/btna_serial.log")
DEFAULT_BAUD = int(os.environ.get("SERIAL_PROXY_BAUD", "115200"))


def discover_port():
    """Prefer a stable /dev/serial/by-id/* symlink, it follows the physical adapter across
    ttyUSBn renumbering on replug, unlike a bare /dev/ttyUSB0. Env override wins."""
    env = os.environ.get("SERIAL_PROXY_PORT")
    if env:
        return env
    byid = "/dev/serial/by-id"
    try:
        links = sorted(os.path.join(byid, e) for e in os.listdir(byid))
        if links:
            return links[0]
    except OSError:
        pass
    for cand in ("/dev/ttyUSB0", "/dev/ttyACM0"):
        if os.path.exists(cand):
            return cand
    return "/dev/ttyUSB0"


DEFAULT_PORT = discover_port()

try:
    import serial  # pyserial
except ImportError:
    serial = None


# ---------- shared state ----------
def load_state():
    try:
        s = json.load(open(STATE))
        return s if _alive(s["pid"]) else None
    except Exception:
        return None


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# ---------- the proxy daemon ----------
class Proxy:
    def __init__(self, port, baud, log, reset):
        self.port, self.baud, self.log, self.reset_on_attach = port, baud, log, reset
        self.ser = None
        self.pause = False
        self.pause_since = None
        self.reset_req = False
        self.stop = False
        self.poller = select.poll()
        self.reg_fd = None
        self.empty = 0

    def _open(self, do_reset):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
        if do_reset:
            self._reset_pulse()
        try:
            self.ser.reset_input_buffer()   # drop power-on glitch bytes on (re)attach
        except Exception:
            pass
        self.poller.register(self.ser.fileno(),
                             select.POLLIN | select.POLLERR | select.POLLHUP | select.POLLNVAL)
        self.reg_fd = self.ser.fileno()
        self.empty = 0

    def _close(self):
        if self.reg_fd is not None:
            try:
                self.poller.unregister(self.reg_fd)
            except (KeyError, OSError):
                pass
            self.reg_fd = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def _reset_pulse(self):
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.1)
            self.ser.reset_input_buffer()
            self.ser.setRTS(False)
            self._w("\n----- proxy: reset pulse %s -----\n" % time.strftime("%T"))
        except Exception:
            pass

    def _w(self, txt):
        with open(self.log, "a", buffering=1) as f:
            f.write(txt)
        sys.stdout.write(txt)
        sys.stdout.flush()

    def run(self):
        signal.signal(signal.SIGUSR1, lambda *_: self._set_pause(True))   # flash: release
        signal.signal(signal.SIGUSR2, lambda *_: self._set_pause(False))  # flash: resume
        signal.signal(signal.SIGHUP, lambda *_: setattr(self, "reset_req", True))
        signal.signal(signal.SIGTERM, lambda *_: setattr(self, "stop", True))
        signal.signal(signal.SIGINT, lambda *_: setattr(self, "stop", True))
        json.dump({"pid": os.getpid(), "port": self.port, "baud": self.baud, "log": self.log},
                  open(STATE, "w"))
        self._open(self.reset_on_attach)
        self._w("\n===== proxy attached%s %s pid=%d  port=%s =====\n" % (
            " + reset" if self.reset_on_attach else "", time.strftime("%F %T"),
            os.getpid(), self.port))
        missing = False
        try:
            while not self.stop:
                if self.pause:
                    if self.ser:
                        self._w("\n----- proxy: released port for flash %s -----\n" % time.strftime("%T"))
                        self._close()
                    if self.pause_since and time.time() - self.pause_since > 180:
                        self._w("\n----- proxy: auto-resume (pause timeout) -----\n")
                        self._set_pause(False)
                    else:
                        time.sleep(0.05)
                        continue
                if not self.ser:
                    # The device went away (power-cycle / replug). Reopening in-process is not
                    # reliable: on re-enumeration the USB-UART bridge comes back at a default
                    # baud and an in-process reopen doesn't always re-apply 115200, giving
                    # garbage. The reliable recovery is a fresh process, so once the device
                    # reappears we re-exec ourselves.
                    if os.path.exists(self.port):
                        self._w("\n----- proxy: device back; restarting clean (re-exec) %s -----\n"
                                % time.strftime("%T"))
                        self._close()
                        try:
                            os.remove(STATE)        # the fresh process rewrites it (same pid)
                        except OSError:
                            pass
                        sys.stdout.flush()
                        time.sleep(0.4)             # let udev settle the new device node
                        os.execv(sys.executable, [sys.executable] + sys.argv)
                    if not missing:
                        self._w("\n----- proxy: %s gone, waiting for it to reappear... -----\n" % self.port)
                        missing = True
                    time.sleep(0.3)
                    continue
                if self.reset_req:
                    self.reset_req = False
                    self._reset_pulse()
                try:
                    events = self.poller.poll(200)
                except Exception:
                    self._close()
                    continue
                if not events:
                    continue
                ev = events[0][1]
                if ev & (select.POLLHUP | select.POLLERR | select.POLLNVAL):
                    self._w("\n----- proxy: device disconnected; reopening %s -----\n" % time.strftime("%T"))
                    self._close()
                    continue
                if ev & select.POLLIN:
                    try:
                        data = self.ser.read(4096)
                    except (OSError, serial.SerialException):
                        self._w("\n----- proxy: read error; reopening %s -----\n" % time.strftime("%T"))
                        self._close()
                        continue
                    if data:
                        self.empty = 0
                        self._w(data.decode("utf-8", "replace"))
                    else:
                        # POLLIN with no bytes == EOF: the device went away
                        self.empty += 1
                        if self.empty > 3:
                            self._w("\n----- proxy: device EOF; reopening %s -----\n" % time.strftime("%T"))
                            self._close()
        finally:
            self._close()
            try:
                os.remove(STATE)
            except OSError:
                pass

    def _set_pause(self, v):
        self.pause = v
        self.pause_since = time.time() if v else None


# ---------- helpers ----------
def _wait_port_free(port, timeout):
    """Poll until the port can be opened (the proxy has released it)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
            os.close(fd)
            return True
        except OSError:
            time.sleep(0.1)
    return False


# ---------- subcommands ----------
def cmd_monitor(a):
    if serial is None:
        sys.exit("pyserial missing: pip install --user pyserial")
    if load_state():
        sys.exit("proxy already running; `serial_proxy.py status`")
    print("[serial_proxy] owning %s -> %s   (tail -f %s)" % (a.port, a.log, a.log), flush=True)
    Proxy(a.port, a.baud, a.log, not a.no_reset).run()


def cmd_flash(a):
    extra = a.extra[1:] if a.extra and a.extra[0] == "--" else a.extra
    cmd = ["pio", "run", "-d", FW_DIR, "-e", a.env, "-t", "upload"] + extra
    s = load_state()
    if s:
        os.kill(s["pid"], signal.SIGUSR1)               # ask proxy to release the port
        _wait_port_free(s["port"], 6)
        print("[serial_proxy] proxy paused; flashing", flush=True)
    rc = 1
    try:
        rc = subprocess.call(cmd)
    finally:
        if s and _alive(s["pid"]):
            if rc == 0 and not a.no_truncate:           # fresh, small log for the new firmware (default)
                try:
                    with open(s["log"], "w") as f:
                        f.write("===== flashed %s @ %s =====\n" % (a.env, time.strftime("%F %T")))
                    print("[serial_proxy] log truncated for new firmware", flush=True)
                except OSError:
                    pass
            os.kill(s["pid"], signal.SIGUSR2)           # resume
            time.sleep(0.4)
            os.kill(s["pid"], signal.SIGHUP)            # reset -> deterministically capture a clean boot
            print("[serial_proxy] proxy resumed + reset -> %s" % s["log"], flush=True)
    sys.exit(rc)


def cmd_reset(a):
    s = load_state()
    if not s:
        sys.exit("no proxy running")
    os.kill(s["pid"], signal.SIGHUP)
    print("[serial_proxy] reset pulse sent")


def cmd_stop(a):
    s = load_state()
    if not s:
        print("no proxy running")
        return
    os.kill(s["pid"], signal.SIGTERM)
    for _ in range(20):
        if not _alive(s["pid"]):
            break
        time.sleep(0.1)
    print("[serial_proxy] stopped pid %d" % s["pid"])


def cmd_status(a):
    s = load_state()
    print("proxy: pid %d  port %s  baud %s  log %s" % (s["pid"], s["port"], s["baud"], s["log"])
          if s else "proxy: not running")


def cmd_tail(a):
    log = (load_state() or {}).get("log", a.log)
    open(log, "a").close()
    os.execvp("tail", ["tail", "-n", "+1", "-F", log])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("monitor", help="own the port + tee to the shared log")
    m.add_argument("--port", default=DEFAULT_PORT)
    m.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    m.add_argument("--log", default=DEFAULT_LOG)
    m.add_argument("--no-reset", action="store_true")
    m.set_defaults(fn=cmd_monitor)

    f = sub.add_parser("flash", help="pause proxy, build+upload, resume (truncates log by default)")
    f.add_argument("--env", default="wroom32")
    f.add_argument("--no-truncate", action="store_true",
                   help="keep the existing log instead of truncating it for the new firmware")
    f.add_argument("extra", nargs=argparse.REMAINDER)
    f.set_defaults(fn=cmd_flash)

    for name, fn in (("reset", cmd_reset), ("stop", cmd_stop), ("status", cmd_status)):
        sp = sub.add_parser(name)
        sp.set_defaults(fn=fn)
    t = sub.add_parser("tail")
    t.add_argument("--log", default=DEFAULT_LOG)
    t.set_defaults(fn=cmd_tail)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
