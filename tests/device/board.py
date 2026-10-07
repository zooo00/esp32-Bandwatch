"""Serial helper for the hardware tier: one long-lived connection to the board, a background reader that keeps
every line, and small wait/collect primitives with timeouts.

Port handling follows CLAUDE.md rule 1: the port is opened with DTR and RTS asserted (no edge, no reset), it is
opened exclusively (so a running host tool is detected instead of fought with), and HUPCL is cleared so closing
it does not drop DTR/RTS either. Nothing here ever toggles the lines or runs esptool.
"""
import base64
import json
import re
import threading
import time

import serial

BAND_NAMES = ("5g", "2.4g", "both", "ble", "154", "spec")
CH_24 = list(range(1, 14))
CH_5 = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
        149, 153, 157, 161, 165]
CH_154 = list(range(11, 27))
EXPECTED_CHS = {"5g": CH_5, "2.4g": CH_24, "both": CH_24 + CH_5, "ble": [], "154": CH_154, "spec": CH_154}
EV_KEYS = {"on", "card", "base", "file", "written", "pending", "surv", "new", "drop", "err", "wait"}
LCD_W, LCD_H = 172, 320
MIN_FREE_HEAP = 24 * 1024          # kMinFreeHeapB, the capture refuse-floor (DEVELOPER.md section 16)
WIFI_ROWS_PER_LINE = 24            # kWifiRowsPerLine in host_proto.cpp sendDevices()

_B64 = rb"[A-Za-z0-9+/]*={0,2}"
P_RE = re.compile(rb"^P (\d+) (-?\d+) (\d+) (\d+) (" + _B64 + rb")$")
S_RE = re.compile(rb"^S (\d+) (" + _B64 + rb")$")
M_RE = re.compile(rb"^M (\d+) (\d+) (\d+) (\d+) (" + _B64 + rb")$")
MF_RE = re.compile(rb"^MF (\d+) ([01])$")
MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


def validate_line(raw):
    """Return None if `raw` (bytes, no newline) is a well-formed protocol line, else a short reason.
    Every non-JSON line type is checked for its exact shape and payload length, so a line truncated by a TX
    buffer overrun (the thing rule 6 forbids) is caught whatever its type."""
    if raw.startswith(b"P "):
        m = P_RE.match(raw)
        if not m:
            return "bad P line"
        try:
            data = base64.b64decode(m.group(5), validate=True)
        except Exception:
            return "P payload not base64"
        return None if len(data) <= int(m.group(4)) else "P payload longer than its frame"
    if raw.startswith(b"S "):
        m = S_RE.match(raw)
        if not m:
            return "bad S line"
        return None if len(base64.b64decode(m.group(2))) == int(m.group(1)) else "S length mismatch"
    if raw.startswith(b"MF"):
        return None if MF_RE.match(raw) else "bad MF line"
    if raw.startswith(b"M "):
        m = M_RE.match(raw)
        if not m:
            return "bad M line"
        x, y, w, h = (int(m.group(i)) for i in range(1, 5))
        if w <= 0 or h <= 0 or x + w > LCD_W or y + h > LCD_H:
            return "M region out of bounds"
        try:
            n = len(base64.b64decode(m.group(5), validate=True))
        except Exception:
            return "M payload not base64"
        return None if n == w * h * 2 else "M payload %d B, want %d" % (n, w * h * 2)
    try:
        # SSIDs/BLE names may carry raw bytes >= 0x80 (only control chars are stripped), so decode leniently,
        # as the host does; invalid UTF-8 is not truncation.
        obj = json.loads(raw.decode("utf-8", "replace"))
    except Exception as e:
        return "not JSON (%s)" % e.__class__.__name__
    if not isinstance(obj, dict) or not isinstance(obj.get("t"), str):
        return "JSON without a string 't'"
    if obj["t"] == "w":
        rows = obj.get("dev")
        if not isinstance(rows, list):
            return "w without a dev list"
        if len(rows) > WIFI_ROWS_PER_LINE:
            return "w line with %d rows (> %d)" % (len(rows), WIFI_ROWS_PER_LINE)
        if any(not isinstance(r, list) or len(r) != 17 for r in rows):
            return "w row without 17 fields"
    return None


class Board:
    def __init__(self, port, baud=115200):
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 0.2
        s.dtr = True          # asserted before open: no edge, no reset (rule 1)
        s.rts = True
        s.exclusive = True    # fail loudly if the host tool (or anything else) holds the port
        s.open()
        self._keep_lines_on_close(s)
        self.ser = s
        self.lines = []       # (monotonic time, raw bytes) for every complete line
        self.cond = threading.Condition()
        self.band = None
        self._stop = False
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    @staticmethod
    def _keep_lines_on_close(s):
        # With HUPCL set the tty drops DTR/RTS on the last close - an edge. Clear it so closing is as quiet as
        # opening. Best effort: termios only exists on POSIX.
        try:
            import termios
            attrs = termios.tcgetattr(s.fileno())
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(s.fileno(), termios.TCSANOW, attrs)
        except Exception:
            pass

    def _read(self):
        buf = b""
        synced = False        # the first fragment after open is very likely the tail of a line: drop it
        while not self._stop:
            try:
                chunk = self.ser.read(4096)
            except Exception:
                if self._stop:
                    return
                raise
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not synced:
                    synced = True
                    continue
                line = line.rstrip(b"\r")
                if not line:
                    continue
                with self.cond:
                    self.lines.append((time.monotonic(), line))
                    if line.startswith(b"{"):
                        self._track(line)
                    self.cond.notify_all()

    def _track(self, line):
        try:
            o = json.loads(line.decode("utf-8", "replace"))
        except Exception:
            return
        if isinstance(o, dict) and o.get("t") in ("hello", "ack") and o.get("band") in BAND_NAMES:
            self.band = o["band"]

    def close(self):
        self._stop = True
        self._thread.join(1.0)
        try:
            self.ser.close()
        except Exception:
            pass

    # ---- primitives ----
    def mark(self):
        with self.cond:
            return len(self.lines)

    def clear(self):
        """Forget everything received so far (keeps memory bounded across long runs)."""
        with self.cond:
            self.lines.clear()

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode())

    def raw_since(self, idx):
        with self.cond:
            return [l for _, l in self.lines[idx:]]

    def json_since(self, idx, t=None):
        out = []
        for raw in self.raw_since(idx):
            if not raw.startswith(b"{"):
                continue
            try:
                o = json.loads(raw.decode("utf-8", "replace"))
            except Exception:
                continue
            if isinstance(o, dict) and (t is None or o.get("t") == t):
                out.append(o)
        return out

    def wait_json(self, pred, timeout, since=None, what="a matching line"):
        """First JSON object received after `since` (default: now) for which pred(obj) is true."""
        idx = self.mark() if since is None else since
        end = time.monotonic() + timeout
        while True:
            with self.cond:
                pending = self.lines[idx:]
                idx = len(self.lines)
            for _, raw in pending:
                if raw.startswith(b"{"):
                    try:
                        o = json.loads(raw.decode("utf-8", "replace"))
                    except Exception:
                        continue
                    if isinstance(o, dict) and pred(o):
                        return o
            left = end - time.monotonic()
            if left <= 0:
                raise AssertionError("timed out after %.1f s waiting for %s" % (timeout, what))
            with self.cond:
                if len(self.lines) == idx:
                    self.cond.wait(min(left, 0.5))

    def command(self, cmd, ack_cmd=None, timeout=5.0):
        """Send `cmd` and return its {"t":"ack","cmd":...} reply."""
        name = ack_cmd or cmd.split()[0]
        idx = self.mark()
        self.send(cmd)
        return self.wait_json(lambda o: o.get("t") == "ack" and o.get("cmd") == name, timeout, idx,
                              "the ack of %r" % cmd)

    def command_or_err(self, cmd, timeout=5.0):
        """Like command(), but also returns an {"t":"err"} reply (e.g. sdcap without a card)."""
        name = cmd.split()[0]
        idx = self.mark()
        self.send(cmd)
        return self.wait_json(lambda o: (o.get("t") == "ack" and o.get("cmd") == name)
                              or (o.get("t") == "err" and str(o.get("msg", "")).startswith(name)),
                              timeout, idx, "a reply to %r" % cmd)

    def hello(self, timeout=5.0):
        idx = self.mark()
        self.send("info")
        return self.wait_json(lambda o: o.get("t") == "hello", timeout, idx, "hello after info")

    def collect(self, seconds):
        """Raw lines received during the next `seconds`."""
        idx = self.mark()
        time.sleep(seconds)
        return self.raw_since(idx)

    def set_band(self, mode, settle=1.5):
        """Switch mode (skips when already there); returns the hello that follows the ack."""
        if self.band == mode:
            return None
        idx = self.mark()
        self.send("band " + mode)
        self.wait_json(lambda o: o.get("t") == "ack" and o.get("cmd") == "band", 15, idx, "band ack")
        h = self.wait_json(lambda o: o.get("t") == "hello", 10, idx, "hello after band")
        time.sleep(settle)    # BLE / 15.4 bring-up
        return h
