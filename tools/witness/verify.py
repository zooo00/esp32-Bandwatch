#!/usr/bin/env python3
"""Witness-backed verification for the Bandwatch deauth investigation (docs/DEVELOPER.md §11).

The device's own counters cannot tell you whether a frame reached the air: `da` is incremented at the
end of sendInternalKick() regardless, and ic_tx_pkt() returns void. The only honest oracle is a second
radio. This drives both boards at once and prints a verdict per experiment:

    C5   (target)  - bandwatch firmware, injects
    S3   (witness) - tools/witness/witness.ino, RX only, never transmits

Usage:
    python3 tools/witness/verify.py                    # run the standard suite
    python3 tools/witness/verify.py --fc 80            # one shot: internal path with FC byte0 = 0x80
    python3 tools/witness/verify.py --ch 6 --secs 8

Stop the host tool first - it holds the C5's serial port.
"""
import argparse, json, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial required:  pip3 install pyserial")

C5_PORT = "/dev/cu.usbmodem1101"
W_PORT  = "/dev/cu.usbmodem21101"


class Link:
    """A serial port opened DTR/RTS-asserted (CLAUDE.md rule 1: toggling them reboots the board)."""

    def __init__(self, port, name):
        self.name = name
        self.s = serial.Serial(port, 115200, timeout=0.2)
        time.sleep(1.2)
        self.s.reset_input_buffer()

    def send(self, cmd, settle=1.2):
        self.s.reset_input_buffer()
        self.s.write((cmd + "\n").encode())
        time.sleep(settle)
        self.s.read(65536)

    def drain(self, secs):
        end, buf = time.time() + secs, b""
        while time.time() < end:
            chunk = self.s.read(8192)
            if chunk:
                buf += chunk
        out = []
        for ln in buf.decode("utf-8", "replace").split("\n"):
            ln = ln.strip()
            if ln.startswith("{"):
                try:
                    out.append(json.loads(ln))
                except ValueError:
                    pass
        return out

    def close(self):
        self.s.close()


def observe(c5, w, secs, expect_src=None):
    """Watch the witness for `secs`. Returns (mgmt frames matching expect_src, last stat)."""
    w.s.write(b"clear\n")
    time.sleep(0.3)
    w.s.reset_input_buffer()
    c5.s.reset_input_buffer()
    msgs = w.drain(secs)
    stat = next((m for m in reversed(msgs) if m.get("t") == "stat"), None)
    rx = [m for m in msgs if m.get("t") == "rx"]
    if expect_src:
        rx = [m for m in rx if m.get("src", "").lower() == expect_src.lower()]
    return rx, stat


def verdict(label, hits, stat, want):
    """want=True  -> we expect frames on air; want=False -> this is a baseline."""
    alive = stat and stat.get("beacon", 0) > 0
    ok = (len(hits) > 0) == want
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {label}")
    print(f"         on air: {len(hits)} matching frame(s)" +
          (f", first={hits[0]['k']} seq={hits[0]['seq']} rssi={hits[0]['rssi']}" if hits else ""))
    if stat:
        print(f"         witness ch{stat['ch']} heard {stat['beacon']} ambient beacons "
              f"({'RX alive' if alive else 'RX DEAD - result is meaningless'})")
    return ok and (alive or not want)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ch", type=int, default=6, help="2.4 GHz channel both radios sit on")
    ap.add_argument("--secs", type=int, default=8, help="observation window per experiment")
    ap.add_argument("--fc", help="one-shot: internal-path FC byte0 in hex (e.g. 80, c0)")
    ap.add_argument("--bssid", default="aa:bb:cc:dd:ee:ff", help="BSSID the C5 spoofs")
    ap.add_argument("--c5", default=C5_PORT)
    ap.add_argument("--witness", default=W_PORT)
    a = ap.parse_args()

    c5 = Link(a.c5, "c5")
    w  = Link(a.witness, "witness")
    try:
        print(f"setting up: C5 -> 2.4 GHz ch{a.ch}, witness -> ch{a.ch}")
        c5.send("band 2.4g", 2.5)
        c5.send(f"park {a.ch}")
        c5.send("deauth 0"); c5.send("txtest 0")
        w.send(f"ch {a.ch}", 0.6)

        results = []

        if a.fc:
            c5.send(f"kickfc {a.fc}")
            c5.send(f"deauth {a.bssid}")
            hits, stat = observe(c5, w, a.secs, expect_src=a.bssid)
            results.append(verdict(f"internal path, FC=0x{a.fc} (expect frames on air)",
                                   hits, stat, want=True))
            c5.send("deauth 0"); c5.send("kickfc c0")
        else:
            # 1. Baseline - nothing injected. Guards against counting ambient traffic as a hit.
            hits, stat = observe(c5, w, 5, expect_src=a.bssid)
            results.append(verdict("baseline, nothing injected (expect silence)",
                                   hits, stat, want=False))

            # 2. Control - esp_wifi_80211_tx beacon. Proves the radio can transmit at all.
            c5.send("txtest 1")
            hits, stat = observe(c5, w, a.secs, expect_src="02:ba:ad:be:ef:01")
            results.append(verdict("control: esp_wifi_80211_tx beacon (expect frames on air)",
                                   hits, stat, want=True))
            c5.send("txtest 0")

            # 3. The question - internal descriptor path carrying a BEACON. If this radiates, the path
            #    works and only the deauth subtype is being dropped. If not, the path itself is dead.
            c5.send("kickfc 80")
            c5.send(f"deauth {a.bssid}")
            hits, stat = observe(c5, w, a.secs, expect_src=a.bssid)
            results.append(verdict("internal path carrying a beacon (FC=0x80)", hits, stat, want=True))
            c5.send("deauth 0")

            # 4. The attack itself.
            c5.send("kickfc c0")
            c5.send(f"deauth {a.bssid}")
            hits, stat = observe(c5, w, a.secs, expect_src=a.bssid)
            results.append(verdict("internal path carrying a deauth (FC=0xC0)", hits, stat, want=True))
            c5.send("deauth 0")

        print("\n" + "=" * 68)
        print(f"{sum(1 for r in results if r)}/{len(results)} checks met expectation")
        print("Anything the witness did not hear did not reach the air, whatever `da` says.")
    finally:
        try:
            c5.send("deauth 0", 0.6); c5.send("txtest 0", 0.6); c5.send("kickfc c0", 0.6)
        except Exception:
            pass
        c5.close(); w.close()


if __name__ == "__main__":
    main()
