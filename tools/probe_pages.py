#!/usr/bin/env python3
"""Page-vs-page heap in `both` band: overview(0) vs channels(1), repeated.

Boot fresh, warm up ~12 s (first sweeps done), then alternate pages and sample 8 s each.
If the gap between pages persists across passes => real per-page LVGL cost; if it closes => first-sweep transient.
"""
import json, os, time
import serial

PORT = os.environ.get("BANDWATCH_PORT", "/dev/cu.usbmodem1101")
s = serial.Serial()
s.port, s.baudrate, s.timeout = PORT, 115200, 0.2
s.dtr = True; s.rts = True   # CLAUDE.md rule 1: open with both asserted or the board resets
s.open()
buf = b""

def pump(secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        try: d = s.read(s.in_waiting or 1)
        except Exception: time.sleep(0.3); continue
        if not d: continue
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try: yield json.loads(line)
            except Exception: pass

def sample(secs, label=""):
    vals = []
    for msg in pump(secs):
        t = msg.get("t")
        if t == "hello":
            print(f"   hello heap={msg.get('heap')} band={msg.get('band')} up={msg.get('up')}"); vals.append(msg.get("heap"))
        elif t in ("s","ble","fs") and msg.get("heap") is not None:
            vals.append(msg["heap"])
    g = sorted(v for v in vals if v)
    return (g[0], g[len(g)//2], g[-1], len(g)) if g else None

def step():
    s.write(b"page next\n")
    got = None
    for m in pump(2.0):
        if m.get("t") == "ack" and m.get("cmd") == "page":
            got = m.get("page"); break
    return got

def send(cmd, wait=0.8):
    s.write(cmd.encode() + b"\n"); time.sleep(wait)

send("band both", 1.5)
for m in pump(2): pass
send("reboot", 0.3); time.sleep(4)
sample(12, "(warmup)")

# now walk pages: expect order 0 -> 1 -> 3 -> 5 -> 0 ...
print("\npass structure: sample 8 s on each of overview/channels/devices/system, twice")
for p in range(2):
    for name in ("overview","channels","devices","system"):
        got = step()
        r = sample(8)
        if r: print(f"pass{p+1} {name:9s} (pg {got})  min={r[0]:6d} med={r[1]:6d} max={r[2]:6d} n={r[3]}")
    # back to overview for a clean loop start
while True:
    got = step()
    if got == 0: break
s.close()
