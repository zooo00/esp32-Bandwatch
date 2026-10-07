#!/usr/bin/env python3
"""Read-only serial smoke check for Bandwatch: a ~15 s "is the board alive and on the build I expect" pass, for
right after a flash. The full on-board suite is tests/device (~5.5 min).

    python3 tools/smoke.py [port]          # port defaults to the first /dev/cu.usbmodem*

It only asks: `info` (hello), `sdinfo`, `sdls`. Nothing that changes the board - no band/park/capture/hunt/deauth,
nothing persisted to NVS, nothing transmitted. (`sdinfo`/`sdls` mount the card briefly to read it, as the
dashboard does.) Stop the host tool first: the port is opened exclusively.

Port handling follows CLAUDE.md rule 1, like tests/device/board.py: DTR and RTS are asserted *before* open (no
edge, no reset) and HUPCL is cleared so closing does not drop them either. Never toggles the lines, never esptool.

Checks, each printed as ok/FAIL; exit status 0 only if all pass:
  - a hello arrives (retried for ~30 s, so it works seconds after a flash) and its version matches kVersion in
    bandwatch/bandwatch_core.h (pass --any-version to skip that)
  - free heap in hello is above the 24 kB capture floor
  - every JSON line seen parses (a truncated line would break rule 6)
  - the radio is reporting: a dwell/sweep/BLE/spectrum status line within a few seconds
  - sdinfo answers; with a card in, sdls lists its files
"""
import glob
import json
import os
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
CORE_H = os.path.join(HERE, "..", "bandwatch", "bandwatch_core.h")
MIN_FREE_HEAP = 24 * 1024            # kMinFreeHeapB (DEVELOPER.md section 16)
STATUS_TYPES = {"d", "s", "ble", "fs"}   # per-mode "the radio is running" lines


def repo_version():
    try:
        m = re.search(r'kVersion\s*=\s*"([^"]+)"', open(CORE_H, encoding="utf-8").read())
        return m.group(1) if m else None
    except OSError:
        return None


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = True          # asserted before open: no edge, no reset (rule 1)
    s.rts = True
    s.exclusive = True    # fail loudly if the host tool holds the port
    s.open()
    try:                  # with HUPCL set, the last close drops DTR/RTS - an edge; clear it
        import termios
        attrs = termios.tcgetattr(s.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(s.fileno(), termios.TCSANOW, attrs)
    except Exception:
        pass
    return s


class Reader:
    def __init__(self, ser):
        self.ser, self.buf, self.bad = ser, b"", []

    def lines(self, secs):
        """Yield decoded JSON objects for `secs` seconds; non-JSON lines (P/S/M/MF, boot text) are skipped,
        JSON-looking lines that fail to parse are recorded in self.bad."""
        end = time.time() + secs
        while time.time() < end:
            self.buf += self.ser.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                raw = raw.strip()
                if not raw.startswith(b"{"):
                    continue
                try:
                    yield json.loads(raw)
                except ValueError:
                    self.bad.append(raw[:120])

    def wait_for(self, pred, secs):
        for o in self.lines(secs):
            if pred(o):
                return o
        return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    any_version = "--any-version" in sys.argv
    port = args[0] if args else next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        sys.exit("No /dev/cu.usbmodem* port found - plug the board in or pass the port.")
    try:
        ser = open_port(port)
    except serial.SerialException as e:
        sys.exit(f"Cannot open {port}: {e}\n(stop host/bandwatch_host.py or any serial monitor first)")

    results = []
    def check(ok, what):
        results.append(ok)
        print(f"{'ok  ' if ok else 'FAIL'}  {what}")

    rd = Reader(ser)
    print(f"Bandwatch smoke check on {port} (read-only)")
    hello = None
    for _ in range(10):                       # ~30 s: the board may still be booting after a flash
        ser.write(b"info\n")
        hello = rd.wait_for(lambda o: o.get("t") == "hello", 3)
        if hello:
            break
    check(hello is not None, "hello received" + ("" if hello else " (no reply in ~30 s)"))
    if not hello:
        ser.close()
        sys.exit(1)

    ver, want = hello.get("ver"), repo_version()
    if any_version or not want:
        check(bool(ver), f"firmware reports version {ver}")
    else:
        check(ver == want, f"firmware {ver} matches repo kVersion {want}"
              + ("" if ver == want else " - flash with ./build.sh --upload"))
    heap = hello.get("heap", 0)
    check(heap >= MIN_FREE_HEAP, f"free heap {heap // 1024} kB (floor {MIN_FREE_HEAP // 1024} kB)")
    print(f"      mode {hello.get('band')}, up {hello.get('up', 0)} s, last reset {hello.get('rst')}")

    st = rd.wait_for(lambda o: o.get("t") in STATUS_TYPES, 8)
    check(st is not None, f"radio reporting ({st.get('t')!r} line)" if st else "radio reporting (no status line in 8 s)")

    ser.write(b"sdinfo\n")
    sdi = rd.wait_for(lambda o: o.get("t") == "ack" and o.get("cmd") == "sdinfo", 5)
    check(sdi is not None, "sdinfo answered" + (f": card {'in, ' + str(sdi.get('mb')) + ' MB' if sdi.get('sd') else 'not present'}"
                                                 if sdi else ""))
    if sdi and sdi.get("sd"):
        ser.write(b"sdls\n")
        ls = rd.wait_for(lambda o: o.get("t") == "sdls", 6)
        check(ls is not None, f"sdls listed {len(ls.get('files', []))} files" if ls else "sdls answered")

    check(not rd.bad, "every JSON line parsed" + (f" ({len(rd.bad)} bad, first: {rd.bad[0]!r})" if rd.bad else ""))
    ser.close()
    passed = all(results)
    print("PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
