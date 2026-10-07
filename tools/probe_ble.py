#!/usr/bin/env python3
"""NimBLE retention probe v5 — repeated W0->B->W1 on one fixed page, delta each round.

Same band (both) + same page in W0 and W1 isolates the BLE controller memory that
deinit(false) leaves behind:  delta = min(W0) - min(W1)  (>0 => RAM permanently held).
"""
import json, os, sys, time
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

def sample(label, secs):
    vals, types = [], {}
    for msg in pump(secs):
        t = msg.get("t")
        if t == "hello":
            print(f"   hello heap={msg.get('heap')} band={msg.get('band')} up={msg.get('up')} rst={msg.get('rst')!r}")
            vals.append(msg.get("heap"))
        elif t in ("s","ble","fs") and msg.get("heap") is not None:
            vals.append(msg["heap"]); types[t] = types.get(t,0)+1
    good = [v for v in vals if v]
    g = sorted(v for v in good)
    if g: print(f"{label:24s} n={len(g):3d} min={g[0]:6d} med={g[len(g)//2]:6d} max={g[-1]:6d}")
    else: print(f"{label:24s} (none) {types}")
    return g

def send(cmd, wait=0.8):
    s.write(cmd.encode() + b"\n"); time.sleep(wait)

def goToPage(target, max_steps=8):
    for _ in range(max_steps):
        s.write(b"page next\n")
        got = None
        for m in pump(2.0):
            if m.get("t") == "ack" and m.get("cmd") == "page":
                got = m.get("page"); break
        print(f"   page -> {got}")
        if got == target: return True
    return False

TGT = int(sys.argv[1]) if len(sys.argv) > 1 else 1     # default PAGE_CHANNELS (available in both band)
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 2

# boot fresh so BT starts cold, on the target page
send("band both", 1.5)
for m in pump(2): pass
send("reboot", 0.3); time.sleep(4)
sample("(warmup)", 6)

results = []
for r in range(ROUNDS):
    print(f"\n--- round {r+1} (target page {TGT}) ---")
    goToPage(TGT); time.sleep(2.5)          # let the (re)built page settle before sampling
    W0 = sample("W0 both, BT cold", 22)
    send("band ble", 1.2); time.sleep(1.5)
    B  = sample("B first BLE visit", 16)
    send("band both", 1.2)
    goToPage(TGT); time.sleep(2.5)
    W1 = sample("W1 both, post-BLE", 22)
    if W0 and W1:
        dmin = min(W0) - min(W1); dmed = sorted(W0)[len(W0)//2] - sorted(W1)[len(W1)//2]
        results.append((dmin, dmed))
        print(f">>> delta  min(W0)-min(W1)={dmin:+6d} B   med-diff={dmed:+6d} B")

if len(results) > 1:
    import statistics as st
    dm = [x[0] for x in results]; de = [x[1] for x in results]
    print(f"\nmean(min-delta)={st.mean(dm):+.0f} B   sd={st.pstdev(dm):.0f} B     mean(med-delta)={st.mean(de):+.0f} B   sd={st.pstdev(de):.0f} B")
    print("(a consistently negative min-delta => W1 has MORE free heap than cold W0: little/no permanent hold)")
s.close()
