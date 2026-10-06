#!/usr/bin/env python3
"""Bandwatch host: reads the board's USB serial stream, serves a live web dashboard, writes pcaps.

    python3 host/bandwatch_host.py                # auto-detects /dev/cu.usbmodem*, serves http://127.0.0.1:8080
    python3 host/bandwatch_host.py --port /dev/cu.usbmodem21101 --http 8080 --captures ./captures
    python3 host/bandwatch_host.py --ui v2        # the new dashboard at "/" (classic stays at /classic); see run-v2.sh

Only needs Python 3 and pyserial (pip install pyserial). The port is opened with DTR/RTS held asserted
(no edges), which is what keeps the ESP32-C5 from resetting when the host connects.

Serial protocol (one line each):
    {"t":"hello", ...}            device info, channel list, band mode              (boot / "info")
    {"t":"d", "c":36, "s":..}     one completed dwell on channel c                  (every ~220 ms)
    {"t":"s", "n":12, "ch":[..]}  full snapshot after every sweep
    {"t":"w", "dev":[...]}        Wi-Fi transmitter table (every 2 s; last field = association suffix)
    {"t":"b", "dev":[...]}        BLE advertiser table (every 2 s, BLE mode)
    {"t":"z", "dev":[...]}        802.15.4 (Zigbee / Thread) node table (every 2 s, 802.15.4 mode)
    {"t":"ble", ...}              BLE-mode heartbeat (every 1 s)
    {"t":"ack"|"log"|"err", ...}
    P <ch> <rssi> <ts_us> <len> <base64 frame>   captured 802.11 frame (when "cap 1")
    S <n> <base64>                               chunk of a file being read back (after "sdread")
    {"t":"sdls","files":[[name,bytes],...],"total":N,"sent":M}   microSD listing ("sdls"); sent<total = truncated mid-list
Commands to the device: "band 5g|2.4g|both|ble|154", "park <ch>|0", "cap 0|1", "snap N", "hunt <mac> [ch]" / "hunt 0",
"deauth <bssid>" / "deauth 0" (Wi-Fi modes; currently does not work, see docs), 
"dca <client_mac> <ap_bssid>" / "dca 0" (targeted deauth to one client),
"sdcap 0|1" (record pcap on the device's microSD), "sdinfo", "sdls", "sdread <path>", "time <epoch>", "info".

pcap link types written: 127 radiotap (Wi-Fi), 283 IEEE 802.15.4-TAP, 256 BLE LL with pseudo-header. BLE
records are advertising packets reconstructed from HCI reports - see docs/DEVELOPER.md section 13.

The device has no RTC, so this tool sends "time <epoch>" on connect; without it the device names SD captures
with a counter and timestamps them from uptime. Device-written filenames use UTC; files written here use
local time.

Vendor names come from the IEEE OUI registry: the first run downloads oui.csv (~3 MB) into ~/.cache/bandwatch/
in the background; until then (or offline) a small built-in table is used.
"""
import argparse
import base64
import csv
import glob
import io
import json
import os
import re
import struct
import sys
import threading
import time
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip3 install pyserial")

HERE = os.path.dirname(os.path.abspath(__file__))
HISTORY_LEN = 600       # ~10 minutes of 1 Hz samples for the global trend
# Spectrum mode (band "spec"): per-bin energy history used to estimate a noise floor and a duty cycle, and
# to flag 2.4 GHz energy no recently-decoded Wi-Fi/BLE/Zigbee emitter explains.
SPEC_HIST_LEN = 40          # recent energy-detect samples kept per 15.4 channel (~2 min of sweeps)
SPEC_FLOOR_MARGIN = 6       # dB above the rolling floor that counts as "energy present"
SPEC_MIN_DUTY = 0.10        # need energy in at least this fraction of recent sweeps to flag a bin
SPEC_KNOWN_AGE_S = 120      # a decoded emitter older than this no longer "explains" a frequency
DEV_HIST_LEN = 120      # per-device RSSI samples (one per device report, ~2 s)
DEV_EXPIRE_S = 600      # forget devices not seen for this long
OUI_URL = "https://standards-oui.ieee.org/oui/oui.csv"
OUI_CACHE = os.path.join(os.path.expanduser("~"), ".cache", "bandwatch", "oui.csv")

# Small fallback OUI table (first 3 bytes, upper-case hex) used until the IEEE registry is cached.
OUI_FALLBACK = {
    "3C22FB": "Apple", "F0D1A9": "Apple", "A4C639": "Apple", "DC2B2A": "Apple", "8C8590": "Apple", "F4F15A": "Apple",
    "BC7574": "Apple", "A85C2C": "Apple", "9C35EB": "Apple", "F0DBE2": "Apple", "D0034B": "Apple", "48A195": "Apple",
    "94E6F7": "Intel", "04D3B0": "Intel", "3C9C0F": "Intel", "8C8CAA": "Intel", "A4B1C1": "Intel", "F8633F": "Intel",
    "3C5AB4": "Google", "F4F5D8": "Google", "F88FCA": "Google", "18B430": "Nest", "001A11": "Google",
    "B827EB": "Raspberry Pi", "DCA632": "Raspberry Pi", "E45F01": "Raspberry Pi",
    "D8F15B": "Espressif", "3C71BF": "Espressif", "A4CF12": "Espressif", "24A160": "Espressif", "7CDFA1": "Espressif",
    "84F3EB": "Espressif", "30AEA4": "Espressif", "C82B96": "Espressif", "4C11AE": "Espressif", "48E729": "Espressif",
    "50C7BF": "TP-Link", "C006C3": "TP-Link", "9C53CD": "TP-Link", "F4EC38": "TP-Link", "F81A67": "TP-Link", "A0F3C1": "TP-Link",
    "788A20": "Ubiquiti", "FCECDA": "Ubiquiti", "24A43C": "Ubiquiti", "D021F9": "Ubiquiti", "E063DA": "Ubiquiti", "68D79A": "Ubiquiti",
    "3C7A8A": "Netgear", "A040A0": "Netgear", "28C68E": "Netgear", "9C3DCF": "Netgear",
    "F0B429": "Xiaomi", "64B473": "Xiaomi", "7811DC": "Xiaomi", "00E04C": "Realtek",
    "40B076": "ASUSTek", "04D4C4": "ASUSTek", "2C4D54": "ASUSTek", "F02F74": "ASUSTek", "1C872C": "ASUSTek",
    "4C5E0C": "MikroTik", "6C3B6B": "MikroTik", "D4CA6D": "MikroTik", "E01F0C": "Huawei", "3C0518": "Huawei", "CC96A0": "Huawei",
    "AC37C9": "Sonos", "5CAAFD": "Sonos", "B8E937": "Sonos", "347E5C": "Sonos",
    "FC65DE": "Amazon", "747548": "Amazon", "A002DC": "Amazon", "F0272D": "Amazon", "0C47C9": "Amazon", "40B4CD": "Amazon",
    "ECB5FA": "Signify (Hue)", "001788": "Philips Lighting", "A8BB50": "WiZ",
    "E0286D": "AVM (FRITZ!)", "3810D5": "AVM (FRITZ!)", "C80E14": "AVM (FRITZ!)", "2C3AFD": "AVM (FRITZ!)", "7CFF4D": "AVM (FRITZ!)",
    "44FE3B": "Arcadyan", "1C3BF3": "Arcadyan", "88B1E1": "Sagemcom", "00259C": "Cisco-Linksys", "C05627": "Belkin",
    "5CE931": "Samsung", "8C71F8": "Samsung", "C4731E": "Samsung", "50B7C3": "Samsung", "BC7E8B": "Samsung",
    "DC7196": "Microsoft", "6045BD": "Microsoft", "A4DA22": "Garmin", "4C4FEE": "OnePlus", "000CE6": "Meru Networks",
    "9C5C8E": "ASUSTek", "AC15A2": "Huawei", "FCB214": "Ubiquiti", "F4A475": "Intel",
}

# Bluetooth SIG company identifiers (subset)
BLE_COMPANY = {
    0x004C: "Apple", 0x0006: "Microsoft", 0x00E0: "Google", 0x0075: "Samsung", 0x0087: "Garmin", 0x0157: "Huami (Amazfit)",
    0x0171: "Amazon", 0x038F: "Xiaomi", 0x0059: "Nordic Semiconductor", 0x02E5: "Espressif", 0x01D7: "Qualcomm",
    0x000F: "Broadcom", 0x000A: "CSR", 0x0002: "Intel", 0x000D: "Texas Instruments", 0x0030: "STMicroelectronics",
    0x0131: "Cypress", 0x0099: "Bose", 0x0310: "Sonos", 0x03DA: "Tile", 0x0046: "Logitech", 0x0110: "Nintendo",
    0x0054: "Sony", 0x012D: "Sony", 0x027D: "Huawei", 0x02FF: "Fitbit", 0x0065: "HP", 0x004F: "Lenovo",
    0x0201: "LG", 0x00C4: "LG", 0x038B: "Withings", 0x0154: "Oura", 0x0B22: "Oura", 0x0553: "Ledger", 0x0BFC: "Tesla",
    0x0343: "Skullcandy", 0x00D2: "Dialog Semiconductor", 0x0369: "Realtek", 0x0057: "Harman", 0x03B4: "Ring (Amazon)",
    0x04C3: "Sennheiser", 0x0180: "Jabra (GN Audio)", 0x0009: "Infineon", 0x0189: "Roku", 0x0451: "Tuya",
    0x0CB2: "Signify (Hue)", 0x010C: "Signify (Hue)", 0x0079: "Philips", 0x003D: "Silicon Labs", 0x02B5: "Silicon Labs",
    0x0129: "Philips", 0x0400: "Netgear", 0x0822: "adidas", 0x01A5: "iFit", 0x0117: "Zepp",
}

APPLE_TYPE = {
    0x02: "iBeacon", 0x05: "AirDrop", 0x06: "HomeKit", 0x07: "AirPods / proximity pairing", 0x08: "Hey Siri",
    0x09: "AirPlay target (Apple TV / HomePod)", 0x0A: "AirPlay source", 0x0B: "Magic Switch (Watch)", 0x0C: "Handoff",
    0x0D: "Tethering target", 0x0E: "Tethering source (iPhone hotspot)", 0x0F: "Nearby Action",
    0x10: "Nearby Info (iPhone / Mac / Watch)", 0x12: "Find My (AirTag / Find My network)", 0x14: "Find My (offline finding)",
    0x16: "Hey Siri", 0x18: "Continuity",
}

APPEARANCE = {
    1: "Phone", 2: "Computer", 3: "Watch", 4: "Clock", 5: "Display", 6: "Remote control", 7: "Eyeglasses", 8: "Tag",
    9: "Keyring", 10: "Media player", 11: "Barcode scanner", 12: "Thermometer", 13: "Heart-rate sensor", 14: "Blood pressure",
    15: "HID (keyboard/mouse/gamepad)", 16: "Glucose meter", 17: "Running/walking sensor", 18: "Cycling sensor", 19: "Control device",
    20: "Network device", 21: "Sensor", 22: "Light fixture", 23: "Fan", 24: "HVAC", 25: "Air conditioning", 26: "Humidifier",
    27: "Heating", 28: "Access control", 29: "Motorized device", 30: "Power device", 31: "Light source", 32: "Window covering",
    33: "Audio sink (speaker)", 34: "Audio source", 35: "Motorized vehicle", 36: "Domestic appliance", 37: "Wearable audio",
    38: "Aircraft", 39: "AV equipment", 40: "Display equipment", 41: "Hearing aid", 42: "Gaming", 43: "Signage",
    49: "Pulse oximeter", 50: "Weight scale", 51: "Personal mobility", 52: "Continuous glucose monitor", 53: "Insulin pump",
    54: "Medication delivery", 55: "Spirometer", 81: "Outdoor sports",
}

SERVICE_UUID = {
    0xFD6F: "Exposure Notification", 0xFEAA: "Eddystone beacon", 0xFE9F: "Google", 0xFE2C: "Google Fast Pair",
    0xFEF3: "Google", 0xFD5A: "Samsung SmartThings", 0xFD69: "Samsung SmartTag", 0x180D: "Heart rate", 0x180F: "Battery",
    0x1812: "HID", 0x1826: "Fitness machine", 0x181C: "User data", 0x1816: "Cycling speed/cadence", 0x1818: "Cycling power",
    0x1814: "Running speed", 0x1810: "Blood pressure", 0x1809: "Health thermometer", 0x1808: "Glucose", 0x181A: "Environmental sensing",
    0xFE07: "Sonos", 0xFEED: "Tile tracker", 0xFD44: "Apple", 0xFE0F: "Philips Hue", 0xFEBE: "Bose", 0xFE61: "Logitech",
    0xFE95: "Xiaomi Mi", 0xFDAB: "Xiaomi", 0xFE78: "HP", 0xFE03: "Amazon", 0xFD3D: "Amazon Sidewalk", 0xFE00: "Amazon",
    0xFDCD: "Qingping", 0xFEE7: "Tencent", 0xFEBB: "Adafruit", 0xFDF0: "Apple", 0x1802: "Immediate alert (find me)",
    0x1803: "Link loss", 0xFE59: "Nordic DFU", 0xFEE0: "Huami (Amazfit)", 0xFDEE: "Huawei", 0xFDD2: "Bosch", 0xFD82: "Sony",
    0xFE9A: "Estimote", 0xFD84: "Tile", 0xFD65: "Razer", 0xFE0D: "Ford", 0xFEF5: "Dialog", 0xFDF7: "HP",
}


MAC_RE = re.compile(r"^[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}$")
# "hunt" also takes an 802.15.4 identifier: an 8-byte extended address, or "pan/short" in hex. Both are
# what the device itself printed in the "z" table, so they round-trip - but they still get checked.
KEY154_RE = re.compile(r"^(?:[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){7}|[0-9a-fA-F]{1,4}/[0-9a-fA-F]{1,4})$")


def clean_mac(v):
    """Accept only a canonical colon-separated MAC, lower-cased. Returns None otherwise.

    send() already collapses newlines, so a value from the HTTP API cannot smuggle a second command onto
    the serial line - but without this a value containing a *space* could still reshape the argument list
    of a two-argument command (a crafted "client_mac" supplying its own "ap_bssid")."""
    if not isinstance(v, str):
        return None
    v = v.strip()
    return v.lower() if MAC_RE.match(v) else None


def clean_hunt_id(v):
    """A hunt target: a Wi-Fi/BLE MAC, or an 802.15.4 extended address / "pan/short"."""
    if not isinstance(v, str):
        return None
    v = v.strip()
    return v.lower() if MAC_RE.match(v) or KEY154_RE.match(v) else None


def find_port():
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbserial*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


# Surveillance-hardware categories, mirrored from the firmware's kSurvOuis table. The device does the
# OUI match (so the LCD can flag too) and reports the category id; this just names it. An OUI match is
# evidence, not proof - prefixes get reassigned, and two Flock prefixes were withdrawn upstream as
# Ubiquiti false positives.
SURV_CAT = {1: "Flock Safety", 2: "Ring", 3: "Axon", 4: "DJI", 5: "Parrot", 6: "Skydio", 7: "Meta/Ray-Ban"}
SURV_KIND = {1: "ALPR camera", 2: "doorbell/camera", 3: "body camera", 4: "drone", 5: "drone",
             6: "drone", 7: "smart glasses"}

PROTO_154 = {0: "802.15.4 (unknown upper layer)", 1: "Zigbee", 2: "Zigbee Green Power", 3: "Thread / 6LoWPAN",
             4: "MAC-layer encrypted (Thread-style)"}


def channel_freq_mhz(ch):
    if ch == 14:
        return 2484
    if ch <= 13:
        return 2407 + 5 * ch
    return 5000 + 5 * ch


class OuiDb:
    def __init__(self):
        self.table = dict(OUI_FALLBACK)
        self.source = "built-in table"
        self.lock = threading.Lock()

    def load_cache(self):
        try:
            with open(OUI_CACHE, newline="", encoding="utf-8", errors="replace") as f:
                self._load(f)
            return True
        except Exception:
            return False

    def _load(self, f):
        tab = dict(OUI_FALLBACK)
        for row in csv.reader(f):
            if len(row) >= 3 and len(row[1]) == 6 and row[0].startswith("MA"):
                tab[row[1].upper()] = row[2].strip()
        with self.lock:
            self.table = tab
            self.source = f"IEEE registry ({len(tab)} prefixes)"

    def download_bg(self):
        def run():
            try:
                os.makedirs(os.path.dirname(OUI_CACHE), exist_ok=True)
                req = urllib.request.Request(OUI_URL, headers={"User-Agent": "bandwatch/1.1"})
                with urllib.request.urlopen(req, timeout=40) as r:
                    data = r.read()
                with open(OUI_CACHE, "wb") as f:
                    f.write(data)
                self._load(io.StringIO(data.decode("utf-8", "replace")))
            except Exception as e:
                with self.lock:
                    self.source = f"built-in table (IEEE download failed: {e.__class__.__name__})"
        threading.Thread(target=run, daemon=True).start()

    def lookup(self, mac):
        try:
            first = int(mac[0:2], 16)
        except ValueError:
            return ""
        if first & 0x02:
            return "(randomized MAC)"
        key = mac.replace(":", "").upper()[:6]
        with self.lock:
            return self.table.get(key, "")


def sec_string(sec, pmf):
    if not sec:
        return ""
    parts = []
    if sec & 0x80: parts.append("Open")
    if sec & 0x40: parts.append("OWE (Enhanced Open)")
    if sec & 0x01: parts.append("WEP")
    if sec & 0x02: parts.append("WPA")
    if sec & 0x04: parts.append("WPA2-PSK")
    if sec & 0x08: parts.append("WPA2-Enterprise")
    if sec & 0x10: parts.append("WPA3-SAE")
    if sec & 0x20: parts.append("WPA3-Enterprise")
    s = " / ".join(parts)
    if pmf == 2: s += " · PMF required"
    elif pmf == 1: s += " · PMF capable"
    return s


def phy_string(phy, ch):
    if not phy:
        return ""
    if phy & 16: return "be (Wi-Fi 7)"
    if phy & 8: return "ax (Wi-Fi 6)"
    if phy & 4: return "ac (Wi-Fi 5)"
    if phy & 2: return "n (Wi-Fi 4)"
    return "a" if ch > 14 else "b/g"


class PcapWriter:
    """pcap writer. Wi-Fi: LINKTYPE_IEEE802_11_RADIOTAP (127) with TSFT, flags, channel and dBm signal per frame.
    802.15.4: LINKTYPE_IEEE802_15_4_TAP (283) with FCS-type, RSS and channel TLVs (Wireshark decodes Zigbee/Thread)."""

    RT_PRESENT = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5)
    RT_LEN = 24
    ADV_ACCESS_ADDR = 0x8E89BED6      # BLE advertising-channel access address

    def __init__(self, path, fcs_present=True, link="wifi"):
        self.path = path
        self.fcs_present = fcs_present
        self.link = link
        self.frames = 0
        self.bytes = 0
        self.f = open(path, "wb")
        linktype = {"154": 283, "ble": 256}.get(link, 127)   # 802.15.4-TAP / BLE LL w/ phdr / radiotap
        self.f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype))
        self.lock = threading.Lock()

    def write(self, ch, rssi, ts_us, orig_len, data):
        if self.link == "ble":
            # LE_LL_WITH_PHDR: channel, signal power, noise, AA offenses, reference AA, flags.
            # CRC-checked/valid bits stay clear - the device synthesizes a zero CRC, and claiming
            # "checked" would make Wireshark flag every frame as CRC-bad. 127 = RSSI not available.
            flags = 0x0011 if rssi == 127 else 0x0013
            rt = struct.pack("<BbBBIH", ch if ch <= 39 else 39, max(-128, min(127, rssi)), 0, 0,
                             self.ADV_ACCESS_ADDR, flags)
        elif self.link == "154":
            # TAP header: version, reserved, total length; TLVs padded to 4 bytes
            rt = struct.pack("<BBH", 0, 0, 28)
            rt += struct.pack("<HHB3x", 0, 1, 0)                                   # FCS type: none (radio strips it)
            rt += struct.pack("<HHf", 1, 4, float(rssi))                            # RSS in dBm
            rt += struct.pack("<HHHBx", 3, 3, ch, 0)                                # channel assignment (page 0)
        else:
            flags = 0x10 if self.fcs_present else 0x00
            band_flags = (0x0100 if ch > 14 else 0x0080) | 0x0040
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
    def __init__(self, port, captures_dir, oui, fcs_present=True):
        self.port_name = port
        self.captures_dir = captures_dir
        self.fcs_present = fcs_present
        self.oui = oui
        self.ser = None
        self.lock = threading.Lock()
        self.dlock = threading.Lock()
        self.pcap = None
        self.state = {
            "connected": False, "port": port, "hello": None, "band": None, "chs": [], "channels": {},
            "current": None, "global": 0.0, "sweep": 0, "aps": 0, "park": 0, "cap": 0, "drop": 0, "heap": None,
            "last_rx": 0, "log": deque(maxlen=60), "capture": None,
            "hunt": None,            # {"mac", "rssi", "age_ms", "count", "hist": deque}
            "deauth": None,          # {"mac", "ch", "sent"} while a deauth attack runs
            "ble": {"devs": 0, "cycles": 0},
            "sd": None,              # {"mounted","mb","cap","file","frames","bytes","err","clock"}
            "sd_read": None,         # a card file being pulled off: {name,total,received,done,path}
            # completed captures this session, per sink, for the dashboard counters
            "saved": {"usb": {"count": 0, "last": None, "frames": 0, "bytes": 0},
                      "sd":  {"count": 0, "last": None, "frames": 0, "bytes": 0}},
        }
        self.wifi_devs = {}
        self.ble_devs = {}
        self.z_devs = {}
        self.cap_band = None
        self.history = deque(maxlen=HISTORY_LEN)
        self._last_hist = 0
        self.spec_hist = {}          # spectrum mode: MHz -> deque of recent edMax (dBm), for the noise floor/duty
        # Fine 2.4 GHz spectrum (spec mode): the device sweeps 2400-2483 MHz in `step` MHz bins via off-grid
        # tuning. bins is a list of [edMin, edMean, edMax, edSamples] in frequency order (lo + i*step).
        self.fine = {"step": 2, "lo": 2400, "count": 0, "bins": [], "current_mhz": 0, "ts": 0}
        # Combined view keeps the 5 GHz packet-activity picture across mode switches (single radio, one at a time).
        self.activity5 = {}; self.activity5_ts = 0   # 5 GHz Wi-Fi ch -> {s,f,b,u,st}

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
            # Collapse embedded newlines/CRs so a value from the HTTP API (e.g. a crafted "mac") can't
            # smuggle a second command onto the serial line.
            line = cmd.strip().replace("\r", " ").replace("\n", " ")
            try:
                self.ser.write((line + "\n").encode())
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
            self.send(f"time {int(time.time())}")   # device has no RTC: pcap timestamps come from this
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

    def _set_hunt(self, mac):
        st = self.state
        if mac and (st["hunt"] is None or st["hunt"]["mac"] != mac):
            st["hunt"] = {"mac": mac, "rssi": None, "age_ms": None, "count": 0, "hist": deque(maxlen=400)}
        elif not mac:
            st["hunt"] = None

    def _sd_track(self, sd):
        """Watch the device's SD block for a recording that has just finished, so the dashboard can
        show a count of completed files and the name of the last one."""
        if not sd:
            return
        prev = getattr(self, "_sd_prev", None)
        if sd.get("cap"):
            # remember progress while it runs; the counters reset when the next capture starts
            self._sd_prev = {"file": sd.get("file") or (prev or {}).get("file"),
                             "frames": sd.get("frames", 0), "bytes": sd.get("bytes", 0)}
        elif prev and prev.get("file"):
            sv = self.state["saved"]["sd"]
            sv["count"] += 1
            sv["last"] = prev["file"]
            sv["frames"] = prev.get("frames", 0)
            sv["bytes"] = prev.get("bytes", 0)
            self.state["log"].append(f"sd capture saved: {prev['file']} ({prev.get('frames', 0)} frames)")
            self._sd_prev = None

    def _set_deauth(self, d):
        # device sends [bssid, park channel (0 if hopping), frames sent, frames failed] or null for broadcast mode;
        # or [client_mac, ap_bssid, targeted_flag=1, sent, fail] for targeted mode.
        # ack lines may carry a bare mac string. fail is optional for compatibility with older firmware.
        st = self.state
        if isinstance(d, str):
            d = [d, 0, 0, 0]
        if d:
            if len(d) >= 5 and d[2] == 1:  # targeted mode: [client_mac, ap_bssid, 1, sent, fail]
                st["deauth"] = {"mac": d[0], "ap_bssid": d[1], "targeted": True, 
                                "sent": d[3], "fail": d[4] if len(d) > 4 else 0}
            elif len(d) >= 3:  # broadcast mode: [bssid, ch, sent, fail]
                st["deauth"] = {"mac": d[0], "ch": d[1], "sent": d[2], 
                                "fail": d[3] if len(d) > 3 else 0, "targeted": False}
        else:
            st["deauth"] = None

    def _hunt_update(self, h):
        hu = self.state["hunt"]
        if h is None or hu is None:
            return
        rssi, age, count = h
        hu["rssi"] = rssi if age < 60000 else None
        hu["age_ms"] = age if age < 60000 else None
        hu["count"] = count
        now = time.time()
        if age < 5000 and (not hu["hist"] or now - hu["hist"][-1][0] >= 0.2):
            hu["hist"].append((round(now, 2), rssi))

    def handle_line(self, raw):
        self.state["last_rx"] = time.time()
        if raw.startswith(b"P "):
            self.handle_frame(raw)
            return
        if raw.startswith(b"S "):
            self.handle_sd_chunk(raw)   # sdread file chunks are not JSON; without this they flood the log pane
            return
        try:
            msg = json.loads(raw.decode("utf-8", "replace"))
        except Exception:
            if raw.strip():
                self.state["log"].append(raw.decode("utf-8", "replace")[:160])
            return
        # A line that parses as JSON but has a field missing or the wrong shape must not escape to
        # reader(): an exception there drops the serial session and silently stops a running capture.
        try:
            self._dispatch(msg)
        except Exception as e:
            self.state["log"].append(f"bad {msg.get('t')!r} line ({e.__class__.__name__}: {e})")

    def _dispatch(self, msg):
        t = msg.get("t")
        st = self.state
        if t == "hello":
            st["hello"] = msg
            # A hold of the BOOT button changes band on the device and switches capture off there; without
            # this the host would keep an open pcap that never grows again (hello is the only line we get).
            if self.pcap and msg.get("band") and self.cap_band != msg.get("band"):
                self.stop_capture()
            st["band"] = msg.get("band")
            st["spec_step"] = msg.get("spec_step", st.get("spec_step", 2))
            st["chs"] = msg.get("chs", [])
            st["park"] = msg.get("park", 0)
            st["cap"] = msg.get("cap", 0)
            st["heap"] = msg.get("heap")
            st["channels"] = {c: v for c, v in st["channels"].items() if c in st["chs"]}
            for c in st["chs"]:
                st["channels"].setdefault(c, {"s": 0.0, "r": 0.0, "f": 0, "b": 0, "st": 0, "u": 0, "state": 1, "t": 0})
            self._set_hunt(msg.get("hunt"))
            self._hunt_update(msg.get("h"))
            self._set_deauth(msg.get("deauth"))
            if msg.get("sd") is not None:
                st["sd"] = msg["sd"]
                self._sd_track(st["sd"])
        elif t == "d":
            c = msg["c"]
            entry = {"s": msg["s"], "r": msg["r"], "f": msg["f"], "b": msg["b"], "st": msg["st"],
                     "u": msg["u"], "state": 0, "t": time.time()}
            e = msg.get("e")   # spectrum: [edMin, edMean, edMax, edSamples] for this dwell
            if e and len(e) >= 4:
                entry.update({"ed_min": e[0], "ed_mean": e[1], "ed_max": e[2], "ed_samples": e[3]})
            st["channels"][c] = entry
            self._cache_wide(st.get("band"), c, entry)
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
            if msg.get("da") is not None and st["deauth"]:
                st["deauth"]["sent"] = msg["da"]
                st["deauth"]["fail"] = msg.get("df", st["deauth"].get("fail", 0))
            if msg.get("sdc") is not None:
                sd = st.get("sd") or {}
                sd.update({"cap": msg["sdc"], "frames": msg.get("sdf", sd.get("frames", 0)),
                           "bytes": msg.get("sdb", sd.get("bytes", 0))})
                st["sd"] = sd
                self._sd_track(sd)
            self._hunt_update(msg.get("h"))
        elif t == "s":
            st["sweep"] = msg["n"]
            st["global"] = msg["g"]
            st["band"] = msg.get("band", st["band"])
            st["aps"] = msg.get("aps", 0)
            st["drop"] = msg.get("drop", 0)
            st["heap"] = msg.get("heap")
            chs = []
            for row in msg["ch"]:
                c, s, f, b, strong, u, state = row[:7]
                chs.append(c)
                prev = st["channels"].get(c, {})
                entry = {"s": s, "r": prev.get("r", s), "f": f, "b": b, "st": strong, "u": u,
                         "state": state, "t": prev.get("t", 0)}
                st["channels"][c] = entry
                self._cache_wide(st["band"], c, entry)   # caches 5 GHz activity for the combined view
            st["chs"] = chs
            st["channels"] = {c: v for c, v in st["channels"].items() if c in chs}
        elif t == "fs":
            # Fine 2.4 GHz spectrum sweep: full set of frequency bins.
            bins = msg.get("bins", [])
            self.fine.update({"step": msg.get("step", 2), "lo": msg.get("lo", 2400),
                              "count": msg.get("count", len(bins)), "bins": bins, "ts": time.time()})
            st["spec_step"] = msg.get("step", st.get("spec_step", 2))
            st["sweep"] = msg.get("n", st.get("sweep", 0))
            st["heap"] = msg.get("heap", st.get("heap"))
            lo, step = self.fine["lo"], self.fine["step"]
            for i, row in enumerate(bins):
                if len(row) >= 4 and row[3]:   # edSamples > 0
                    self._spec_track(lo + i * step, row[2])   # track edMax per MHz for the noise floor
        elif t == "fd":
            # Fine spectrum per-dwell: which frequency the sweep is on now + that bin's energy (walking cursor).
            self.fine["current_mhz"] = msg.get("mhz", 0)
            self.fine["step"] = msg.get("step", self.fine["step"])
            st["spec_step"] = msg.get("step", st.get("spec_step", 2))
            st["sweep"] = msg.get("n", st.get("sweep", 0))
        elif t == "w":
            self.merge_wifi(msg.get("dev", []))
        elif t == "z":
            self.merge_154(msg.get("dev", []))
        elif t == "b":
            self.merge_ble(msg.get("dev", []))
        elif t == "ble":
            st["ble"] = {"devs": msg.get("devs", 0), "cycles": msg.get("cycles", 0),
                         "adv": msg.get("adv", 0), "scan": msg.get("scan"),
                         "running": msg.get("running"), "switches": msg.get("switches", 0)}
            if msg.get("cap") is not None:
                st["cap"] = msg["cap"]
            if msg.get("drop") is not None:
                st["drop"] = msg["drop"]
            # BLE mode has no dwell lines, so this heartbeat is the only live SD progress the host sees
            if msg.get("sdc") is not None:
                sd = st.get("sd") or {}
                sd.update({"cap": msg["sdc"], "frames": msg.get("sdf", sd.get("frames", 0)),
                           "bytes": msg.get("sdb", sd.get("bytes", 0))})
                st["sd"] = sd
                self._sd_track(sd)
            st["heap"] = msg.get("heap", st["heap"])
            self._hunt_update(msg.get("h"))
        elif t == "ack":
            st["log"].append("ack " + json.dumps({k: v for k, v in msg.items() if k != "t"}))
            if "band" in msg:
                st["band"] = msg["band"]
                if self.pcap and self.cap_band != msg["band"]:
                    self.stop_capture()   # link type differs per radio: a new capture starts a new file
            if "park" in msg:
                st["park"] = msg["park"]
            if "cap" in msg:
                st["cap"] = msg["cap"]
            if msg.get("cmd") == "hunt":
                self._set_hunt(msg.get("hunt"))
            if msg.get("cmd") in ("deauth", "dca"):
                # "dca" too: without it a targeted attack left st["deauth"] at None, so the deauth card
                # never appeared and the dwell handler (which only updates an existing entry) never
                # showed a frame count - the attack ran, invisibly, until the next "hello".
                self._set_deauth(msg.get("deauth"))
            if msg.get("cmd") in ("sdcap", "sdinfo"):
                sd = st.get("sd") or {}
                # the device's ack uses "sdcap" for the running flag and "sd" for card presence
                keymap = {"sd": "mounted", "sdcap": "cap", "cap": "cap", "mb": "mb", "file": "file",
                          "frames": "frames", "bytes": "bytes", "err": "err"}
                for k, dest in keymap.items():
                    if k in msg:
                        sd[dest] = msg[k]
                st["sd"] = sd
                self._sd_track(sd)
            if msg.get("cmd") == "sdread":
                self._sd_read_start(msg.get("file"), msg.get("bytes"))   # the ack precedes the S chunks
            elif msg.get("cmd") == "sdread_done":
                self._sd_read_finish(True)
        elif t == "sdls":
            # One-shot listing. hello replaces st["sd"] wholesale (no files field), so the dashboard re-asks
            # whenever the card is present and the list is missing; sent<total means it was truncated.
            # (The device reports total/sent, not the listed/skipped pair 1.5.4 used on the other branch —
            # the dashboard renders the file list itself rather than dumping it into the log pane.)
            sd = st.get("sd") or {}
            sd["files"] = [[n, sz] for n, sz in msg.get("files", [])]
            sd["file_total"] = msg.get("total", len(sd["files"]))
            st["sd"] = sd
        elif t in ("log", "err"):
            st["log"].append(f"{t}: {msg.get('msg')}")
            if str(msg.get("msg", "")).startswith("sdread"):
                self._sd_read_finish(False)   # the file will not be coming; close out the half-built buffer

    def merge_wifi(self, rows):
        now = time.time()
        with self.dlock:
            for r in rows:
                try:
                    mac, rssi, mx, frames, age, ch, flags, ssid = r[:8]
                    extra = r[8:15] if len(r) >= 15 else [0, 0, 0, 0, 0, 0, ""]
                    surv = r[15] if len(r) >= 16 else 0
                    ap_suffix = r[16] if len(r) >= 17 else ""
                except Exception:
                    continue
                d = self.wifi_devs.get(mac)
                if d is None:
                    d = {"mac": mac, "first": now, "hist": deque(maxlen=DEV_HIST_LEN), "ssid": "", "sec": "", "phy": "",
                         "bw": None, "util": None, "stations": None, "cc": ""}
                    self.wifi_devs[mac] = d
                d["vendor"] = self.oui.lookup(mac)
                # tier 2 = we heard it transmit; tier 1 = only ever seen as a frame destination (addr1)
                dest_only = bool(flags & 4)
                d.update({"rssi": rssi, "max": mx, "frames": frames, "last": now - age / 1000.0, "ch": ch,
                          "ap": bool(flags & 1), "ssid": ssid or d["ssid"],
                          "dest_only": dest_only, "ap_suffix": ap_suffix or d.get("ap_suffix", ""),
                          "surv": SURV_CAT.get(surv, ""), "surv_kind": SURV_KIND.get(surv, ""),
                          "tier": (0 if not surv else 1 if dest_only else 2)})
                sec, pmf, phy, bw, util, stations, cc = extra
                if flags & 2:
                    d.update({"sec": sec_string(sec, pmf), "phy": phy_string(phy, ch), "bw": bw * 10 if bw else None,
                              "util": round(util * 100 / 255) if util else None, "stations": stations, "cc": cc})
                if age < 4000 and (not d["hist"] or now - d["hist"][-1][0] >= 1.5):
                    d["hist"].append((round(now, 1), rssi))
            self._expire(self.wifi_devs, now)
            self._resolve_parents()

    def _resolve_parents(self):
        """Turn each station's 3-byte association suffix into the full BSSID of an AP we actually know.

        The device only has room for the last three BSSID bytes (see devices.h), so the join is completed
        here: a station is attributed to an AP only when that AP is in our own table and its low 24 bits
        agree, and never when two known APs share those bits. Caller holds self.dlock."""
        by_suffix = {}
        for d in self.wifi_devs.values():
            if d.get("ap"):
                by_suffix.setdefault(d["mac"].replace(":", "")[-6:], []).append(d["mac"])
        for d in self.wifi_devs.values():
            sfx = d.get("ap_suffix") or ""
            hits = by_suffix.get(sfx, []) if sfx else []
            # Exactly one candidate, and never an AP pointing at itself.
            d["parent"] = hits[0] if len(hits) == 1 and hits[0] != d["mac"] else None

    def merge_ble(self, rows):
        now = time.time()
        with self.dlock:
            for r in rows:
                try:
                    mac, rssi, mx, adv, age, atype, company, name = r[:8]
                    extra = r[8:14] if len(r) >= 14 else [0, 127, 0, 0, 0, 0]
                    bsurv = r[14] if len(r) >= 15 else 0
                except Exception:
                    continue
                appearance, tx, svc, svcdata, apple, flags = extra
                d = self.ble_devs.get(mac)
                if d is None:
                    d = {"mac": mac, "first": now, "hist": deque(maxlen=DEV_HIST_LEN), "name": ""}
                    self.ble_devs[mac] = d
                random_addr = bool(atype) or bool(int(mac[0:2], 16) & 0x02)
                vendor = BLE_COMPANY.get(company) or ("" if random_addr else self.oui.lookup(mac))
                kinds = []
                if apple:
                    kinds.append(APPLE_TYPE.get(apple, f"Apple type 0x{apple:02x}"))
                if appearance:
                    kinds.append(APPEARANCE.get(appearance >> 6, f"appearance 0x{appearance:04x}"))
                for u in (svc, svcdata):
                    if u:
                        kinds.append(SERVICE_UUID.get(u, f"service 0x{u:04x}"))
                d.update({"rssi": rssi, "max": mx, "adv": adv, "last": now - age / 1000.0, "random": random_addr,
                          "company": company, "company_name": BLE_COMPANY.get(company, f"0x{company:04x}" if company else ""),
                          "vendor": vendor, "name": name or d["name"], "appearance": appearance,
                          "tx": None if tx == 127 else tx, "svc": svc, "svcdata": svcdata, "apple": apple,
                          "connectable": bool(flags & 1), "legacy": bool(flags & 2), "kind": ", ".join(dict.fromkeys(kinds)),
                          "surv": SURV_CAT.get(bsurv, ""), "surv_kind": SURV_KIND.get(bsurv, ""),
                          "tier": 2 if bsurv else 0})
                if age < 4000 and (not d["hist"] or now - d["hist"][-1][0] >= 1.5):
                    d["hist"].append((round(now, 1), rssi))
            self._expire(self.ble_devs, now)

    def merge_154(self, rows):
        now = time.time()
        with self.dlock:
            for r in rows:
                try:
                    key, rssi, mx, frames, age, ch, pan, short, proto, flags, lqi = r[:11]
                except Exception:
                    continue
                d = self.z_devs.get(key)
                if d is None:
                    d = {"key": key, "first": now, "hist": deque(maxlen=DEV_HIST_LEN)}
                    self.z_devs[key] = d
                ext = bool(flags & 1)
                d.update({"rssi": rssi, "max": mx, "frames": frames, "last": now - age / 1000.0, "ch": ch,
                          "pan": None if pan == 0xFFFF else f"{pan:04x}", "short": None if short == 0xFFFF else f"{short:04x}",
                          "ext": key if ext else "", "vendor": self.oui.lookup(key) if ext else "",
                          "proto": PROTO_154.get(proto, "?"), "proto_id": proto,
                          "beacons": bool(flags & 2), "permit_join": bool(flags & 4), "mac_secured": bool(flags & 8),
                          "data": bool(flags & 16), "lqi": lqi})
                if age < 4000 and (not d["hist"] or now - d["hist"][-1][0] >= 1.5):
                    d["hist"].append((round(now, 1), rssi))
            self._expire(self.z_devs, now)

    @staticmethod
    def _expire(table, now):
        for mac in [m for m, d in table.items() if now - d["last"] > DEV_EXPIRE_S]:
            del table[mac]

    def _cache_wide(self, band, c, entry):
        """Cache the 5 GHz packet-activity picture for the combined view (the single radio scans one band at a
        time, so 5 GHz is shown as last-seen while 2.4 GHz energy is live). 2.4 GHz energy lives in self.fine."""
        if band in ("5g", "both") and c > 14 and "s" in entry:   # 5 GHz Wi-Fi channels only
            with self.dlock:   # read by _wide() on HTTP threads
                self.activity5[c] = {"s": entry["s"], "f": entry.get("f", 0), "b": entry.get("b", 0),
                                     "u": entry.get("u", 0), "st": entry.get("st", 0)}
                self.activity5_ts = time.time()

    def _wide(self):
        now = time.time()
        with self.dlock:
            return {
                "activity5": [dict(ch=c, freq=5000 + 5 * c, **self.activity5[c]) for c in sorted(self.activity5)],
                "activity5_age": round(now - self.activity5_ts, 1) if self.activity5_ts else None,
            }

    def _spec_track(self, mhz, ed_max):
        with self.dlock:   # read by _unidentified() on HTTP threads; keyed by frequency (MHz)
            h = self.spec_hist.get(mhz)
            if h is None:
                h = deque(maxlen=SPEC_HIST_LEN)
                self.spec_hist[mhz] = h
            h.append(ed_max)

    def _unidentified(self):
        """Flag 2.4 GHz energy bins carrying sustained power that no recently-decoded Wi-Fi/BLE/Zigbee
        emitter explains. The single radio can't decode and energy-scan at once, so "known" is drawn from
        the last time those modes ran: this is an unexplained-energy flag, not a device identification."""
        now = time.time()
        known = []   # (lo_mhz, hi_mhz) spans we have actually decoded recently
        with self.dlock:
            for d in self.wifi_devs.values():
                ch = d.get("ch") or 0
                if 1 <= ch <= 14 and now - d["last"] <= SPEC_KNOWN_AGE_S:
                    f = 2412 + 5 * (ch - 1)
                    known.append((f - 11, f + 11))   # ~22 MHz occupied bandwidth
            for d in self.z_devs.values():
                ch = d.get("ch") or 0
                if 11 <= ch <= 26 and now - d["last"] <= SPEC_KNOWN_AGE_S:
                    f = 2405 + 5 * (ch - 11)
                    known.append((f - 1, f + 1))
            ble_recent = any(now - d["last"] <= SPEC_KNOWN_AGE_S for d in self.ble_devs.values())
        if ble_recent:
            # BLE advertises on 2402/2426/2480 and hops data channels across the band; if we've decoded any
            # BLE recently, treat at least the advertising frequencies as explained.
            for f in (2402, 2426, 2480):
                known.append((f - 1, f + 1))
        # Snapshot the per-bin history under the lock (the reader thread appends to these deques), then work
        # off the copy. Noise floor = the band-wide quiet level (a low percentile across all bins and recent
        # sweeps), so a continuously-on emitter is still measured against ambient quiet, not its own history.
        with self.dlock:
            hist = {mhz: list(h) for mhz, h in self.spec_hist.items()}   # keyed by frequency (MHz)
        all_samples = sorted(v for h in hist.values() for v in h)
        if len(all_samples) < 8:
            return []
        floor = all_samples[max(0, int(len(all_samples) * 0.15) - 1)]
        gate = floor + SPEC_FLOOR_MARGIN
        out = []
        for mhz, h in hist.items():
            if not h:
                continue
            peak = max(h)
            duty = sum(1 for v in h if v > gate) / len(h)
            if peak <= gate or duty < SPEC_MIN_DUTY:
                continue
            if any(lo <= mhz <= hi for lo, hi in known):
                continue
            out.append({"freq_mhz": mhz, "peak_dbm": peak, "floor_dbm": floor, "duty": round(duty, 2)})
        out.sort(key=lambda e: e["peak_dbm"], reverse=True)
        return out

    def handle_frame(self, raw):
        pcap = self.pcap          # stop_capture() runs on the HTTP thread and may clear/close it mid-frame
        if pcap is None:
            return
        try:
            parts = raw.split(b" ", 5)
            ch, rssi, ts_us, orig_len = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            data = base64.b64decode(parts[5])
            pcap.write(ch, rssi, ts_us, orig_len, data)
        except Exception:
            return
        cap = self.state["capture"]
        if cap:
            cap["frames"] = pcap.frames
            cap["bytes"] = pcap.bytes

    # ---------------- sdread reassembly ----------------
    def _sd_read_start(self, name, total):
        """The 'sdread' ack precedes the chunks and carries the file's size."""
        self._sd_read = {"name": name or "bandwatch.pcap", "total": total or 0, "buf": bytearray()}
        self.state["sd_read"] = {"name": os.path.basename(self._sd_read["name"]), "total": self._sd_read["total"],
                                 "received": 0, "done": False}

    def _sd_read_finish(self, done):
        r = getattr(self, "_sd_read", None)
        if not r:
            return
        self._sd_read = None
        st = dict(self.state.get("sd_read") or {})
        st["received"] = len(r["buf"])
        st["done"] = done
        if done and r["buf"]:   # write it next to the USB captures; same name as on the card, overwritten on re-pull
            path = os.path.join(self.captures_dir, os.path.basename(r["name"]))
            try:
                with open(path, "wb") as f:
                    f.write(bytes(r["buf"]))
                st["path"] = path
                self.state["log"].append(f"sd file pulled: {os.path.basename(r['name'])} ({len(r['buf'])} bytes)")
            except Exception as e:
                st.pop("path", None)
                self.state["log"].append(f"sd pull could not write the file: {e}")
        self.state["sd_read"] = st

    def handle_sd_chunk(self, raw):
        """One 'S <n> <base64>' chunk of an sdread file streaming off the card (device: serviceSdRead)."""
        r = getattr(self, "_sd_read", None)
        if not r:
            return   # chunks without a start ack have nowhere to land
        try:
            data = base64.b64decode(raw.split(b" ", 2)[2])
        except Exception:
            return
        with self.dlock:   # snapshot() reads the summary on HTTP threads; keep the counter honest
            r["buf"] += data
            if self.state.get("sd_read") is not None:
                self.state["sd_read"]["received"] = len(r["buf"])

    # ---------------- control ----------------
    def start_capture(self, snaplen=None):
        if self.pcap:
            return self.state["capture"]
        os.makedirs(self.captures_dir, exist_ok=True)
        band = self.state["band"]
        link = "154" if band == "154" else "ble" if band == "ble" else "wifi"
        path = os.path.join(self.captures_dir,
                            time.strftime("bandwatch-%s-%%Y%%m%%d-%%H%%M%%S.pcap"
                                          % ({"154": "802154", "ble": "ble"}.get(link, "wifi"))))
        self.pcap = PcapWriter(path, fcs_present=self.fcs_present, link=link)
        self.cap_band = band
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
            sv = self.state["saved"]["usb"]
            sv["count"] += 1
            sv["last"] = self.pcap.path
            sv["frames"] = self.pcap.frames
            sv["bytes"] = self.pcap.bytes
            self.pcap = None
        self.state["capture"] = None

    def hunt(self, mac, ch=None):
        if not mac:
            self.send("hunt 0")
            return
        mac = mac.strip().lower()
        if ch is None:
            d = self.wifi_devs.get(mac) or self.z_devs.get(mac)
            ch = d["ch"] if d else 0
        self.send(f"hunt {mac} {int(ch or 0)}")

    def snapshot(self):
        st = self.state
        now = time.time()
        chans = [dict(ch=c, **st["channels"].get(c, {})) for c in st["chs"]]
        with self.dlock:
            wifi = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1)) for d in self.wifi_devs.values()]
            ble = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1)) for d in self.ble_devs.values()]
            zig = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1)) for d in self.z_devs.values()]
            src = None
            if st["hunt"]:
                src = self.wifi_devs.get(st["hunt"]["mac"]) or self.ble_devs.get(st["hunt"]["mac"]) or self.z_devs.get(st["hunt"]["mac"])
        hunt = None
        if st["hunt"]:
            hu = st["hunt"]
            hunt = {"mac": hu["mac"], "rssi": hu["rssi"], "age_ms": hu["age_ms"], "count": hu["count"],
                    "hist": list(hu["hist"]), "label": (src or {}).get("ssid") or (src or {}).get("name") or (src or {}).get("proto") or "",
                    "vendor": (src or {}).get("vendor", ""), "kind": (src or {}).get("kind", "") or ((src or {}).get("pan") and "PAN " + src["pan"]) or ""}
        return {
            "connected": st["connected"], "port": st["port"],
            "age": round(now - st["last_rx"], 1) if st["last_rx"] else None,
            "band": st["band"], "current": st["current"], "global": st["global"], "sweep": st["sweep"], "aps": st["aps"],
            "park": st["park"], "cap": st["cap"], "drop": st["drop"], "heap": st["heap"], "hello": st["hello"],
            "channels": chans, "history": list(self.history), "capture": st["capture"],
            "unidentified": self._unidentified() if st["band"] == "spec" else [],
            "wide": self._wide(),
            "spec_step": st.get("spec_step", 2),
            "fine": {"step": self.fine["step"], "lo": self.fine["lo"], "count": self.fine["count"],
                     "current_mhz": self.fine["current_mhz"],
                     "age": round(now - self.fine["ts"], 1) if self.fine["ts"] else None,
                     "bins": [{"mhz": self.fine["lo"] + i * self.fine["step"],
                               "min": r[0], "mean": r[1], "max": r[2], "n": r[3]}
                              for i, r in enumerate(self.fine["bins"]) if len(r) >= 4]},
            "captures_dir": os.path.abspath(self.captures_dir), "log": list(st["log"])[-15:],
            "wifi_devs": wifi, "ble_devs": ble, "z_devs": zig, "hunt": hunt, "deauth": st["deauth"], "ble": st["ble"], "sd": st["sd"],
             "sd_read": st["sd_read"], "saved": st["saved"],
            "oui_source": self.oui.source,
        }


def make_handler(bw, classic_path, v2_path=None, ui="classic"):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
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
            if path == "/file":   # capture pulled off the card (or over USB) for download
                name = (parse_qs(urlparse(self.path).query).get("name") or [""])[0]
                fpath = os.path.join(bw.captures_dir, name)
                ok = bool(name) and len(name) <= 128 and "/" not in name and ".." not in name \
                    and os.path.isfile(fpath)   # a plain basename: it cannot escape the captures dir
                if not ok:
                    self.send_response(404)
                    self.end_headers()
                    return
                with open(fpath, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            # Both dashboards are always served; --ui only decides which one sits at "/".
            page = None
            if path in ("/", "/index.html"):
                page = v2_path if ui == "v2" and v2_path else classic_path   # falls back to classic if v2 is missing
            elif path in ("/classic", "/classic/"):
                page = classic_path
            elif path in ("/v2", "/v2/", "/v2.html") and v2_path:
                page = v2_path
            if page and os.path.isfile(page):
                with open(page, "rb") as f:
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
            try:
                if cmd == "band" and req.get("value") in ("5g", "2.4g", "both", "ble", "154", "spec"):
                    bw.send(f"band {req['value']}")
                elif cmd == "specstep" and int(req.get("value") or 0) in (1, 2, 5):
                    bw.send(f"specstep {int(req['value'])}")
                elif cmd == "park":
                    bw.send(f"park {int(req.get('value') or 0)}")
                elif cmd == "capture":
                    if req.get("value"):
                        bw.start_capture(req.get("snaplen"))
                    else:
                        bw.stop_capture()
                elif cmd == "hunt":
                    target = clean_hunt_id(req.get("mac"))
                    if req.get("mac") and not target:
                        return self._json({"error": "bad hunt target"}, 400)
                    bw.hunt(target, req.get("ch"))
                elif cmd == "deauth":
                    mac = clean_mac(req.get("mac"))
                    if req.get("mac") and not mac:
                        return self._json({"error": "bad mac"}, 400)
                    bw.send(f"deauth {mac or '0'}")   # the device finds the AP's channel itself
                elif cmd == "dca":  # targeted deauth to one specific client
                    mac = clean_mac(req.get("client_mac") or req.get("mac"))
                    ap_bssid = clean_mac(req.get("ap_bssid"))
                    if (req.get("client_mac") or req.get("mac") or req.get("ap_bssid")) and not (mac and ap_bssid):
                        return self._json({"error": "dca needs a valid client_mac and ap_bssid"}, 400)
                    bw.send(f"dca {mac} {ap_bssid}" if mac and ap_bssid else "dca 0")
                elif cmd == "sdcap":
                    bw.send(f"sdcap {1 if req.get('value') else 0}")
                elif cmd == "sdread":
                    name = (req.get("path") or "").lstrip("/")
                    # The card lists bare names ("bandwatch-wifi-..."), but SD.open wants an absolute path, so we
                    # accept either and normalize to "/name". "sdread /" is 8 chars and the device's line buffer
                    # holds 47, so the name must be <= 39 or its last char is silently dropped on the way in.
                    if re.match(r"^bandwatch-(?:wifi|ble|802154)-\S+\.pcap$", name) and len(name) <= 39:
                        bw.send(f"sdread /{name}")
                    else:
                        return self._json({"error": "bad card file path"}, 400)
                elif cmd == "addr1":
                    bw.send(f"addr1 {1 if req.get('value') else 0}")
                elif cmd == "blescan" and req.get("value") in ("passive", "active", "auto"):
                    bw.send(f"blescan {req['value']}")
                elif cmd == "sdinfo":
                    bw.send("sdinfo")
                elif cmd == "sdls":
                    bw.send("sdls")
                elif cmd == "info":
                    bw.send("info")
                else:
                    return self._json({"error": "unknown command"}, 400)
            except (TypeError, ValueError) as e:
                return self._json({"error": f"bad argument: {e}"}, 400)
            return self._json({"ok": True})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--http", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--ui", choices=("classic", "v2"), default="classic",
                    help="which dashboard sits at '/' — the other stays reachable at /classic or /v2 (default: classic)")
    ap.add_argument("--bind", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    ap.add_argument("--captures", default=os.path.join(os.getcwd(), "captures"), help="pcap output directory")
    ap.add_argument("--no-fcs", action="store_true", help="do not mark frames as carrying an FCS in radiotap")
    ap.add_argument("--no-oui-download", action="store_true", help="do not fetch the IEEE OUI registry")
    args = ap.parse_args()

    oui = OuiDb()
    if not oui.load_cache() and not args.no_oui_download:
        oui.download_bg()

    bw = Bandwatch(args.port, args.captures, oui, fcs_present=not args.no_fcs)
    threading.Thread(target=bw.reader, daemon=True).start()
    html_classic = os.path.join(HERE, "dashboard.html")
    v2_candidate = os.path.join(HERE, "dashboard2.html")
    html_v2 = v2_candidate if os.path.isfile(v2_candidate) else None   # the new UI is optional until promoted
    srv = ThreadingHTTPServer((args.bind, args.http), make_handler(bw, html_classic, html_v2, args.ui))
    other = "/classic" if args.ui == "v2" else "/v2"
    print(f"Bandwatch host: dashboard at http://{args.bind}:{args.http}/ ({'new UI' if args.ui == 'v2' else 'classic UI'}, "
          f"{other} has the other)  (serial: {args.port or 'auto'}, captures: {os.path.abspath(args.captures)}, vendors: {oui.source})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        bw.stop_capture()


if __name__ == "__main__":
    main()
