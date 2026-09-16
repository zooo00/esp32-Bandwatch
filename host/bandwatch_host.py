#!/usr/bin/env python3
"""Bandwatch host: reads the board's USB serial stream, serves a live web dashboard, writes pcaps.

    python3 host/bandwatch_host.py                # auto-detects /dev/cu.usbmodem*, serves http://127.0.0.1:8080
    python3 host/bandwatch_host.py --port /dev/cu.usbmodem21101 --http 8080 --captures ./captures

Only needs Python 3 and pyserial (pip install pyserial). The port is opened with DTR/RTS held asserted
(no edges), which is what keeps the ESP32-C5 from resetting when the host connects.

Serial protocol (one line each):
    {"t":"hello", ...}            device info, channel list, band mode              (boot / "info")
    {"t":"d", "c":36, "s":..}     one completed dwell on channel c                  (every ~220 ms)
    {"t":"s", "n":12, "ch":[..]}  full snapshot after every sweep
    {"t":"ack"|"log"|"err", ...}
    P <ch> <rssi> <ts_us> <len> <base64 frame>   captured 802.11 frame (when "cap 1")
Commands to the device: "band 5g|2.4g|both", "park <ch>|0", "cap 0|1", "snap N", "info".
"""
import argparse
import base64
import glob
import json
import os
import struct
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip3 install pyserial")

HERE = os.path.dirname(os.path.abspath(__file__))
HISTORY_LEN = 600  # ~10 minutes of dwell samples at 1/s for the global sparkline


def find_port():
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbserial*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def channel_freq_mhz(ch):
    if ch == 14:
        return 2484
    if ch <= 13:
        return 2407 + 5 * ch
    return 5000 + 5 * ch


class PcapWriter:
    """pcap (LINKTYPE_IEEE802_11_RADIOTAP = 127) with TSFT, flags, channel and dBm signal per frame."""

    RT_PRESENT = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5)  # TSFT, Flags, Channel, dBm antsignal
    RT_LEN = 24  # 8 hdr + 8 tsft + 1 flags + 1 pad + 4 channel + 1 antsignal + 1 pad

    def __init__(self, path, fcs_present=True):
        self.path = path
        self.fcs_present = fcs_present
        self.frames = 0
        self.bytes = 0
        self.f = open(path, "wb")
        self.f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 127))
        self.lock = threading.Lock()

    def write(self, ch, rssi, ts_us, orig_len, data):
        flags = 0x10 if self.fcs_present else 0x00
        band_flags = 0x0100 if ch > 14 else 0x0080  # 5 GHz / 2 GHz spectrum
        band_flags |= 0x0040  # OFDM (good enough for display purposes)
        rt = struct.pack("<BBHI", 0, 0, self.RT_LEN, self.RT_PRESENT)
        rt += struct.pack("<Q", ts_us)
        rt += struct.pack("<BB", flags, 0)
        rt += struct.pack("<HH", channel_freq_mhz(ch), band_flags)
        rt += struct.pack("<bB", max(-128, min(127, rssi)), 0)
        now = time.time()
        sec, usec = int(now), int((now - int(now)) * 1_000_000)
        rec = struct.pack("<IIII", sec, usec, len(rt) + len(data), len(rt) + orig_len) + rt + data
        with self.lock:
            self.f.write(rec)
            self.frames += 1
            self.bytes += len(data)

    def close(self):
        with self.lock:
            self.f.close()


class Bandwatch:
    def __init__(self, port, captures_dir, fcs_present=True):
        self.port_name = port
        self.captures_dir = captures_dir
        self.fcs_present = fcs_present
        self.ser = None
        self.lock = threading.Lock()
        self.pcap = None
        self.state = {
            "connected": False,
            "port": port,
            "hello": None,
            "band": None,
            "chs": [],
            "channels": {},        # ch -> {s, r, f, b, st, u, state, t}
            "current": None,
            "global": 0.0,
            "sweep": 0,
            "aps": 0,
            "park": 0,
            "cap": 0,
            "drop": 0,
            "heap": None,
            "last_rx": 0,
            "log": deque(maxlen=40),
            "capture": None,       # {file, frames, bytes, started}
        }
        self.history = deque(maxlen=HISTORY_LEN)  # (t, global, current_ch, current_score)
        self._last_hist = 0

    # ---------------- serial side ----------------
    def open(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.5
        # The ESP32-C5's USB-Serial-JTAG turns DTR/RTS edges into BOOT/EN pulses. macOS asserts both lines
        # on open; keeping them asserted (no edge) leaves the board running. Dropping them reboots it.
        s.dtr = True
        s.rts = True
        s.open()
        return s

    def send(self, cmd):
        with self.lock:
            if self.ser is None:
                return False
            try:
                self.ser.write((cmd.strip() + "\n").encode())
                return True
            except Exception as e:
                self.state["log"].append(f"write failed: {e}")
                return False

    def reader(self):
        while True:
            port = self.port_name or find_port()
            if not port:
                self.state["connected"] = False
                time.sleep(1.0)
                continue
            try:
                ser = self.open(port)
            except Exception as e:
                self.state["connected"] = False
                self.state["log"].append(f"open {port}: {e}")
                time.sleep(1.5)
                continue
            with self.lock:
                self.ser = ser
            self.state["connected"] = True
            self.state["port"] = port
            self.state["log"].append(f"connected to {port}")
            self.send("info")
            buf = b""
            try:
                while True:
                    chunk = ser.read(4096)
                    if not chunk:
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self.handle_line(line.rstrip(b"\r"))
            except Exception as e:
                self.state["log"].append(f"serial error: {e}")
            finally:
                with self.lock:
                    self.ser = None
                self.state["connected"] = False
                try:
                    ser.close()
                except Exception:
                    pass
                if self.pcap:
                    self.stop_capture()
            time.sleep(1.0)

    def handle_line(self, raw):
        self.state["last_rx"] = time.time()
        if raw.startswith(b"P "):
            self.handle_frame(raw)
            return
        try:
            text = raw.decode("utf-8", "replace")
            msg = json.loads(text)
        except Exception:
            if raw.strip():
                self.state["log"].append(raw.decode("utf-8", "replace")[:160])
            return
        t = msg.get("t")
        st = self.state
        if t == "hello":
            st["hello"] = msg
            st["band"] = msg.get("band")
            st["chs"] = msg.get("chs", [])
            st["park"] = msg.get("park", 0)
            st["cap"] = msg.get("cap", 0)
            st["heap"] = msg.get("heap")
            # keep only channels that are part of the current band mode
            st["channels"] = {c: v for c, v in st["channels"].items() if c in st["chs"]}
            for c in st["chs"]:
                st["channels"].setdefault(c, {"s": 0.0, "r": 0.0, "f": 0, "b": 0, "st": 0, "u": 0, "state": 1, "t": 0})
        elif t == "d":
            c = msg["c"]
            st["channels"][c] = {"s": msg["s"], "r": msg["r"], "f": msg["f"], "b": msg["b"], "st": msg["st"],
                                 "u": msg["u"], "state": 0, "t": time.time()}
            st["current"] = c
            st["global"] = msg["g"]
            st["sweep"] = msg["n"]
            st["park"] = msg["park"]
            st["cap"] = msg["cap"]
            st["drop"] = msg["drop"]
            now = time.time()
            if now - self._last_hist >= 1.0:
                self.history.append((round(now, 1), msg["g"], c, msg["s"]))
                self._last_hist = now
        elif t == "s":
            st["sweep"] = msg["n"]
            st["global"] = msg["g"]
            st["band"] = msg.get("band", st["band"])
            st["aps"] = msg.get("aps", 0)
            st["drop"] = msg.get("drop", 0)
            st["heap"] = msg.get("heap")
            chs = []
            for row in msg["ch"]:
                c, s, f, b, strong, u, state = row
                chs.append(c)
                prev = st["channels"].get(c, {})
                st["channels"][c] = {"s": s, "r": prev.get("r", s), "f": f, "b": b, "st": strong, "u": u,
                                     "state": state, "t": prev.get("t", 0)}
            st["chs"] = chs
            st["channels"] = {c: v for c, v in st["channels"].items() if c in chs}
        elif t == "ack":
            st["log"].append("ack " + json.dumps({k: v for k, v in msg.items() if k != "t"}))
            if "band" in msg:
                st["band"] = msg["band"]
            if "park" in msg:
                st["park"] = msg["park"]
            if "cap" in msg:
                st["cap"] = msg["cap"]
        elif t in ("log", "err"):
            st["log"].append(f"{t}: {msg.get('msg')}")

    def handle_frame(self, raw):
        if self.pcap is None:
            return
        try:
            parts = raw.split(b" ", 5)
            ch, rssi, ts_us, orig_len = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            data = base64.b64decode(parts[5])
        except Exception:
            return
        self.pcap.write(ch, rssi, ts_us, orig_len, data)
        cap = self.state["capture"]
        if cap:
            cap["frames"] = self.pcap.frames
            cap["bytes"] = self.pcap.bytes

    # ---------------- capture control ----------------
    def start_capture(self, snaplen=None):
        if self.pcap:
            return self.state["capture"]
        os.makedirs(self.captures_dir, exist_ok=True)
        name = time.strftime("bandwatch-%Y%m%d-%H%M%S.pcap")
        path = os.path.join(self.captures_dir, name)
        self.pcap = PcapWriter(path, fcs_present=self.fcs_present)
        self.state["capture"] = {"file": path, "frames": 0, "bytes": 0, "started": time.time()}
        if snaplen:
            self.send(f"snap {int(snaplen)}")
        self.send("cap 1")
        self.state["log"].append(f"capture started: {path}")
        return self.state["capture"]

    def stop_capture(self):
        self.send("cap 0")
        if self.pcap:
            self.pcap.close()
            self.state["log"].append(f"capture stopped: {self.pcap.path} ({self.pcap.frames} frames)")
            self.pcap = None
        cap = self.state["capture"]
        if cap:
            cap["stopped"] = time.time()
        self.state["capture"] = None
        return cap

    def snapshot(self):
        st = self.state
        chans = [dict(ch=c, **st["channels"].get(c, {})) for c in st["chs"]]
        return {
            "connected": st["connected"],
            "port": st["port"],
            "age": round(time.time() - st["last_rx"], 1) if st["last_rx"] else None,
            "band": st["band"],
            "current": st["current"],
            "global": st["global"],
            "sweep": st["sweep"],
            "aps": st["aps"],
            "park": st["park"],
            "cap": st["cap"],
            "drop": st["drop"],
            "heap": st["heap"],
            "hello": st["hello"],
            "channels": chans,
            "history": list(self.history),
            "capture": st["capture"],
            "captures_dir": os.path.abspath(self.captures_dir),
            "log": list(st["log"])[-15:],
        }


def make_handler(bw, html_path):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlparse(self.path).path
            if path == "/api/state":
                return self._json(bw.snapshot())
            if path in ("/", "/index.html"):
                with open(html_path, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(404)
            self.end_headers()

        def do_POST(self):
            path = urlparse(self.path).path
            n = int(self.headers.get("Content-Length", "0") or 0)
            try:
                req = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                return self._json({"error": "bad json"}, 400)
            if path != "/api/cmd":
                self.send_response(404)
                self.end_headers()
                return
            cmd = req.get("cmd")
            if cmd == "band" and req.get("value") in ("5g", "2.4g", "both"):
                bw.send(f"band {req['value']}")
            elif cmd == "park":
                bw.send(f"park {int(req.get('value') or 0)}")
            elif cmd == "capture":
                if req.get("value"):
                    bw.start_capture(req.get("snaplen"))
                else:
                    bw.stop_capture()
            elif cmd == "info":
                bw.send("info")
            else:
                return self._json({"error": "unknown command"}, 400)
            return self._json({"ok": True})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--http", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--bind", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    ap.add_argument("--captures", default=os.path.join(os.getcwd(), "captures"), help="pcap output directory")
    ap.add_argument("--no-fcs", action="store_true", help="do not mark frames as carrying an FCS in radiotap")
    args = ap.parse_args()

    bw = Bandwatch(args.port, args.captures, fcs_present=not args.no_fcs)
    threading.Thread(target=bw.reader, daemon=True).start()
    html_path = os.path.join(HERE, "dashboard.html")
    srv = ThreadingHTTPServer((args.bind, args.http), make_handler(bw, html_path))
    print(f"Bandwatch host: dashboard at http://{args.bind}:{args.http}/  (serial: {args.port or 'auto'}, "
          f"captures: {os.path.abspath(args.captures)})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        bw.stop_capture()


if __name__ == "__main__":
    main()
