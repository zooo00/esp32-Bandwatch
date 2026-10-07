#!/usr/bin/env python3
"""Power profile: walk the board through its operating modes so the current can be read off an inline USB meter.

The board has no current sensor and macOS only reports the 500 mA USB *allocation*, so the draw has to be measured
externally: an inline USB-C power meter (shows mA / mWh) or an INA219 on the 5 V line. This script does the
tedious part - it puts the board in each state, holds it there, and tells you when to read the meter - then prints
a battery-life table from the numbers you type in.

    BANDWATCH_PORT=/dev/cu.usbmodemXXXX python3 tools/power_profile.py [--hold 45] [--auto]

Stop host/bandwatch_host.py first (it holds the serial port). The port is opened with DTR/RTS asserted (CLAUDE.md
rule 1), so the board does not reset. --auto skips the prompts and just holds each state (for a logging meter).
The board is restored to its starting band, capture and event-log state at the end.

Read the *average* (most meters show it, or watch ~10 s and eyeball the middle): Wi-Fi hopping draws in bursts.
"""
import argparse, json, os, sys, time
import serial

PORT = os.environ.get("BANDWATCH_PORT", "/dev/cu.usbmodem1101")

# (label, commands to enter the state, note)
STATES = [
    ("5 GHz scan", ["band 5g"], "Wi-Fi RX, hopping 5 GHz"),
    ("2.4 GHz scan", ["band 2.4g"], "Wi-Fi RX, hopping 2.4 GHz"),
    ("Both bands", ["band both"], "Wi-Fi RX, all 38 channels (default)"),
    ("BLE scan", ["band ble"], "NimBLE discovery, continuous"),
    ("802.15.4", ["band 154"], "Zigbee/Thread RX"),
    ("Spectrum", ["band spec"], "15.4 energy detect sweep"),
    ("Both + SD capture", ["band both", "sdcap 1"], "card writing pcap (needs a card)"),
    ("Both + event log", ["band both", "sdcap 0", "events 1"], "event log armed (card mounts once a minute)"),
]


class Board:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = True; self.s.rts = True   # rule 1: asserted before open, never toggled
        self.s.open()
        self.buf = b""

    def lines(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.buf += self.s.read(self.s.in_waiting or 1)
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                if l.startswith(b"{"):
                    try: yield json.loads(l)
                    except ValueError: pass

    def send(self, cmd, wait=1.0):
        self.s.write((cmd + "\n").encode())
        return list(self.lines(wait))

    def hello(self):
        for m in self.send("info", 2.0):
            if m.get("t") == "hello": return m
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hold", type=int, default=45, help="seconds to hold each state (default 45)")
    ap.add_argument("--auto", action="store_true", help="no prompts; just hold each state")
    a = ap.parse_args()

    b = Board(PORT)
    h0 = b.hello()
    if not h0:
        sys.exit("no hello from the board - wrong port, or the host tool is still running?")
    start_band, start_events = h0.get("band", "both"), (h0.get("ev") or {}).get("on", 0)
    card = bool((h0.get("sd") or {}).get("mounted"))
    print(f"Bandwatch {h0.get('ver')} on {PORT}; start band {start_band}; card {'yes' if card else 'no'}")
    print("Mirror and USB capture stay off (they add serial traffic, not a field-use state).\n")
    b.send("mirror 0"); b.send("cap 0")

    results = []
    try:
        for label, cmds, note in STATES:
            if "sdcap 1" in cmds and not card:
                print(f"-- {label}: skipped (no card)"); continue
            for c in cmds: b.send(c, 1.5)
            print(f"== {label}  ({note})")
            settle = 8
            for _ in b.lines(settle): pass
            print(f"   holding {a.hold} s - read the meter's average now")
            heap = None
            for m in b.lines(a.hold):
                if m.get("heap") is not None: heap = m["heap"]
            if a.auto:
                results.append((label, None))
            else:
                v = input("   mA at 5 V (blank to skip): ").strip()
                results.append((label, float(v) if v else None))
            if heap: print(f"   (free heap {heap} B)")
    finally:
        b.send("sdcap 0", 2.0)
        b.send(f"events {1 if start_events else 0}", 2.0)
        b.send(f"band {start_band}", 3.0)
        print("\nBoard restored.")

    measured = [(l, ma) for l, ma in results if ma]
    if not measured: return
    print("\nBattery life from a USB power bank (5 V output):")
    print(f"{'state':22s} {'mA':>6s} {'W':>6s}  " + "  ".join(f"{c} mAh" for c in (2000, 5000, 10000)))
    for l, ma in measured:
        hrs = ["%5.1f h" % (cap * 0.63 / ma) for cap in (2000, 5000, 10000)]
        print(f"{l:22s} {ma:6.0f} {ma * 5 / 1000:6.2f}  " + "    ".join(hrs))
    print("\n(power-bank mAh are rated at the 3.7 V cell: x 3.7/5 V x ~85% boost efficiency = ~0.63 of the label at 5 V)")


if __name__ == "__main__":
    main()
