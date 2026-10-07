"""Shared fixtures for the offline host tests: build a Bandwatch host object with no serial port, feed it
protocol lines exactly as the reader thread would, and record what it would have written to the device.

The host constructor never opens a port (reader() does that, in its own thread), so no product change was
needed to construct it: we just never start reader(), and plug a FakeSerial into `bw.ser` so send() works.
"""
import json
import os
import sys
import tempfile
import threading

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOST_DIR = os.path.join(REPO, "host")
if HOST_DIR not in sys.path:
    sys.path.insert(0, HOST_DIR)

import bandwatch_host as bh  # noqa: E402  (needs the sys.path entry above)


class FakeSerial:
    """Stands in for a pyserial port: records every line send() writes."""

    def __init__(self):
        self.written = []
        self.lock = threading.Lock()

    def write(self, data):
        with self.lock:
            self.written.append(data.decode())
        return len(data)

    def lines(self):
        with self.lock:
            return [w.rstrip("\n") for w in self.written]

    def clear(self):
        with self.lock:
            self.written.clear()


def make_bw(captures_dir=None, with_serial=True):
    """A host object wired to a FakeSerial (or to no port at all when with_serial=False)."""
    if captures_dir is None:
        captures_dir = tempfile.mkdtemp(prefix="bw-test-")
    bw = bh.Bandwatch(None, captures_dir, bh.OuiDb())
    if with_serial:
        bw.ser = FakeSerial()
    return bw


def feed(bw, line):
    """Hand one protocol line to the host's line handler (dict -> JSON, str -> utf-8, bytes as-is)."""
    if isinstance(line, dict):
        line = json.dumps(line, separators=(",", ":"))
    if isinstance(line, str):
        line = line.encode("utf-8")
    bw.handle_line(line)


class FakeClock:
    """Patch target for time.time(): tests advance it by hand."""

    def __init__(self, start=1_800_000_000.0):
        self.now = float(start)

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


HELLO_24 = {
    "t": "hello", "fw": "bandwatch", "ver": "1.18", "dwell_ms": 220, "spec_step": 2, "band": "2.4g",
    "country": "ESP_OK", "bandmode": "ESP_OK", "proto": "ESP_OK", "promisc": "ESP_OK",
    "chs": list(range(1, 14)), "park": 0, "cap": 0, "snap": 256, "heap": 88123, "up": 12, "rst": "poweron",
    "hunt": None, "h": None, "deauth": None,
    "sd": {"mounted": 1, "mb": 30436, "cap": 0, "file": "", "frames": 0, "bytes": 0, "err": 0, "clock": 1},
    "mir": 0,
    "ev": {"on": 1, "card": 1, "base": 120, "file": 120, "written": 3, "pending": 0, "surv": 1, "new": 2,
           "drop": 0, "err": 0, "wait": 0},
}


def wifi_row(mac, rssi=-50, mx=-45, frames=10, age=500, ch=6, flags=0, ssid="", sec=0, pmf=0, phy=0, bw=0,
             util=0, stations=0, cc="", surv=0, ap_suffix=""):
    """One {"t":"w"} row in the device's 17-field order (host_proto.cpp sendDevices)."""
    return [mac, rssi, mx, frames, age, ch, flags, ssid, sec, pmf, phy, bw, util, stations, cc, surv, ap_suffix]
