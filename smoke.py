#!/usr/bin/env python3
"""Serial smoke test for Bandwatch (steps 1-4 are behavior-neutral). Run with the board plugged in."""
import serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"

def rd(s, secs):
    end = time.time() + secs
    out = b""
    while time.time() < end:
        c = s.read(4096)
        if c:
            out += c
    return out.decode("utf-8", "replace").strip()

def cmd(s, c, wait=1.2):
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    time.sleep(wait)
    return rd(s, wait).replace("\n", " | ")[:400]

s = serial.Serial(PORT, 115200, timeout=0.4)   # opening asserts DTR+RTS -> CDCOnBoot reset
time.sleep(0.6)
print("HELLO :", rd(s, 3)[:400].replace("\n", " | "))

tests = [
    ("info",      1.5),   # hello + sweep? + devices
    ("time 1758624000", 0.8),
    ("cap 1",     1.0),   # ring alloc log
    ("park 60",   1.0),
    ("hunt aa:bb:cc:dd:ee:ff", 1.0),
    ("deauth aa:bb:cc:dd:ee:ff", 1.2),
    ("deauth 0",  0.8),
    ("sdcap 1",   1.2),   # no card -> err, with card -> file ack
    ("sdls",      1.2),
    ("sdcap 0",   0.8),
    ("band ble",  2.5),   # hello for BLE mode
    ("band 5g",   2.0),
    ("park 0",    0.8),
    ("hunt 0",    0.8),
    ("cap 0",     1.0),
]
for c, w in tests:
    print(f"CMD {c!r:34} -> {cmd(s, c, w)}")

s.close()
