#!/usr/bin/env python3
"""Bandwatch host: reads the board's USB serial stream, serves a live web dashboard, writes pcaps.

    python3 host/bandwatch_host.py                # auto-detects /dev/cu.usbmodem*, serves http://127.0.0.1:8080
    python3 host/bandwatch_host.py --port /dev/cu.usbmodem21101 --http 8080 --captures ./captures

The dashboard (dashboard2.html) is served at "/" (also at /v2 for old links). The classic dashboard was removed
in v1.19 (the v1.18.2 tag has the last copy); /classic now redirects to "/" and --ui is accepted but ignored.

Only needs Python 3 and pyserial (pip install pyserial). The port is opened with DTR/RTS held asserted
(no edges), which is what keeps the ESP32-C5 from resetting when the host connects.

Serial protocol (one line each):
    {"t":"hello", ...}            device info, channel list, band mode              (boot / "info")
    {"t":"d", "c":36, "s":..}     one completed dwell on channel c                  (every ~220 ms)
    {"t":"s", "n":12, "ch":[..]}  full snapshot after every sweep
    {"t":"w", "dev":[...]}        Wi-Fi transmitter table (every 2 s; last field = association suffix)
    {"t":"b", "dev":[...]}        BLE advertiser table (every 2 s, BLE mode)
    {"t":"z", "dev":[...]}        802.15.4 (Zigbee / Thread) node table (every 2 s, 802.15.4 mode)
    {"t":"pr", "mac":.., "ssid":..}  a directed probe request: a client asking for that network (C1;
                                  each (MAC, SSID) pair at most once a minute; Wi-Fi modes)
    {"t":"ble", ...}              BLE-mode heartbeat (every 1 s)
    {"t":"ev", "ev":{...}}        SD event log status (every 5 s while armed; the same "ev" object rides on
                                  hello and the "events" ack): on, card, base, file, written, pending,
                                  surv, new, drop, err, wait - see _set_events()
    {"t":"pt", "pt":{...}|null}   C5 patrol status (every 2 s while patrolling, in every mode, and once when it
                                  stops; the same "pt" member rides on hello and the "patrol" ack): leg, left
                                  (ms), cyc, legs [[mode, sec], ...] - see _set_patrol()
    {"t":"ack"|"log"|"err", ...}
    P <ch> <rssi> <ts_us> <len> <base64 frame>   captured 802.11 frame (when "cap 1")
    S <n> <base64>                               chunk of a file being read back (after "sdread")
    M <x> <y> <w> <h> <base64 RGB565-LE>         one repainted LCD region (while "mirror 1")
    MF <seq> <complete>                          end of one LCD refresh (frame marker; complete=1: no region
                                                 dropped and no repair pending, the frame is tear-free).
                                                 Regions are published to /screen.bin only at markers;
                                                 firmware without markers is published per region.
    {"t":"sdls","files":[[name,bytes],...],"total":N,"sent":M}   microSD listing ("sdls"); sent<total = truncated mid-list
Commands to the device: "band 5g|2.4g|both|ble|154|spec", "park <ch>|0", "cap 0|1", "snap N", "hunt <mac> [ch]" /
"hunt 0", "huntssid <name>" / "huntssid 0" (C7: hunt a network name, 1..32 bytes, exact and case-sensitive; acked
with the hunt ack plus "ssid"), "deauth <bssid>" / "deauth 0" (Wi-Fi modes; reaches the air only on the raw-TX-patched image that
build.sh flashes by default - confirm with tools/witness/verify.py, never from the counter; docs/DEVELOPER.md 11),
"dca <client_mac> <ap_bssid>" / "dca 0" (targeted deauth to one client),
"sdcap 0|1" (record pcap on the device's microSD), "sdinfo", "sdls", "sdread <path>", "sdrm <path>" (delete one
card-root file; the pulled local copy stays), "time <epoch>", "info",
"events 0|1" (C4: arm/disarm the SD event log - /events.csv rows for surveillance hits and MACs new to this card's
/seen.csv baseline; persists on the device, and arming with no card just buffers and retries every 30 s),
"alerts 0|1" (LED alert blips for surveillance hits / permit-join / new devices; persists), "ledtest surv|new|join",
"specstep 1|2|5" (fine-spectrum step, spec mode), "blescan active|passive|auto", "addr1 0|1" (track addr1-only
destinations), "mirror 0|1" (stream the LCD), "page next|prev" (step the LCD like a BOOT tap),
"patrol 1|0" / "patrol <mode>:<sec>,..." (C5: walk the modes round-robin; 2-6 legs of 5-600 s; refused while a
capture, hunt or deauth runs, and those are refused while it walks; a "band" ends it).
"explain full|current|clear" on /api/cmd is host-side: it walks the device through Wi-Fi/BLE/15.4 and back to spec.

HTTP API: GET /api/state, /api/screen, /screen.bin, /file?name=..; POST /api/cmd with a JSON object body and
Content-Type: application/json. Errors are non-2xx {"ok": false, "error": ...}; 503 when no device is connected.
The Host header must name the bind address, localhost, 127.0.0.1 or [::1] (DNS-rebinding guard), and a POST with a
foreign Origin is refused. There is no authentication: binding to anything but loopback exposes deauth to the LAN.

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
import copy
import csv
import glob
import io
import json
import os
import re
import shutil
import signal
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
# "Explain" orchestration leg durations (seconds the device spends decoding each mode before returning to spec).
EXPLAIN_DUR_WIFI_FULL = 6.0   # full 2.4 GHz Wi-Fi sweep (13 channels hopping) - a couple of beacon intervals
EXPLAIN_DUR_154_FULL  = 5.0   # full 802.15.4 sweep (channels 11-26)
EXPLAIN_DUR_BLE       = 5.0   # BLE discovery window
EXPLAIN_DUR_PARK      = 3.5   # a single parked Wi-Fi/15.4 channel (targeted "current" scope)
EXPLAIN_SETTLE_S      = 0.3   # let the radio come up before the dwell timing starts to matter
DEV_HIST_LEN = 120      # per-device RSSI samples (one per device report, ~2 s)
DEV_EXPIRE_S = 600      # forget devices not seen for this long
PROBE_TTL_S = 900       # C1: forget a (MAC, SSID) probe pairing not re-announced for this long (device repeats <= 1/min)
MIRROR_HOLD_S = 0.5     # LCD mirror: hold a torn (MF complete=0) frame back at most this long before showing it
MIRROR_LEGACY_REGIONS = 256   # LCD mirror: this many M lines with no MF marker = firmware without markers
MIRROR_STALE_S = 0.5    # LCD mirror: regions left unpublished this long after the last line are published anyway
PROBE_MAX = 512         # cap on probing MACs kept (randomized MACs make one per probe burst)
PROBE_SSIDS_MAX = 32    # cap on SSIDs remembered per probing MAC (the stalest is dropped)
DEV_MAX = 1024          # cap per device table (wifi/ble/15.4): a MAC-randomizing flood evicts the stalest entry
SERIAL_LINE_MAX = 64 * 1024   # a serial "line" longer than this (no newline in sight) is discarded whole
SERIAL_WRITE_TIMEOUT_S = 1.0  # a stalled USB write gives up instead of hanging every HTTP command
HTTP_BODY_MAX = 65536   # largest POST body /api/cmd accepts (413 above)
PCAP_FLUSH_S = 1.0      # flush a running pcap at least this often, so a crash loses at most ~1 s
# C4 event-log text files on the card that "sdread" may pull besides the pcaps (they land in the captures dir under
# the same name, overwritten on re-pull, and download through /file like a pcap).
CARD_TEXT_FILES = ("events.csv", "events.old.csv", "seen.csv", "seen.old.csv", "surveil.csv")
CARD_PCAP_RE = re.compile(r"^bandwatch-(?:wifi|ble|802154)-[^\s/\\]+\.pcap$")


def card_file_name(path):
    """The bare card-root name for an "sdread"/"sdrm" request, or None when it is not one we may touch.

    The card lists bare names ("bandwatch-wifi-..."), but SD.open wants an absolute path, so either form is
    accepted (one leading "/") and the caller sends "/name". Only the card root, only our own files: a pcap the
    device named, or one of CARD_TEXT_FILES - so no "..", no nested path, no other file on the card. The name
    must be <= 39 chars: "sdread /" is 8 chars and the device's line buffer holds 47, so a longer name loses its
    last char on the way in (and "sdrm /" would then delete a different, truncated name)."""
    if not isinstance(path, str):
        return None
    name = path[1:] if path.startswith("/") else path
    if not name or len(name) > 39 or "/" in name or "\\" in name or ".." in name:
        return None
    return name if (CARD_PCAP_RE.match(name) or name in CARD_TEXT_FILES) else None
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


PATROL_MODES = ("5g", "2.4g", "both", "ble", "154", "spec")
PATROL_LEGS_MIN, PATROL_LEGS_MAX = 2, 6     # bandwatch_core.h kPatrolMinLegs / kPatrolMaxLegs
PATROL_SEC_MIN, PATROL_SEC_MAX = 5, 600     # kPatrolSecMin / kPatrolSecMax


def clean_patrol_legs(v):
    """C5 patrol legs, as a list of [mode, sec] pairs (or {"mode", "sec"} objects) or the device's own
    "mode:sec,mode:sec" string. Returns the device argument ("spec:30,both:40") or None when anything is off:
    2..6 legs, modes from PATROL_MODES, whole seconds 5..600. Strict, so nothing malformed reaches the serial line
    (the device re-validates and would refuse it with an err line the dashboard only sees in the log)."""
    if isinstance(v, str):
        v = [part.split(":", 1) if ":" in part else [part, None] for part in v.strip().split(",")]
    if not isinstance(v, list) or not PATROL_LEGS_MIN <= len(v) <= PATROL_LEGS_MAX:
        return None
    out = []
    for leg in v:
        if isinstance(leg, dict):
            mode, sec = leg.get("mode"), leg.get("sec")
        elif isinstance(leg, (list, tuple)) and len(leg) == 2:
            mode, sec = leg
        else:
            return None
        if not isinstance(mode, str) or mode.strip() not in PATROL_MODES:
            return None
        if isinstance(sec, str) and sec.strip().isdigit():
            sec = int(sec.strip())
        if isinstance(sec, bool) or not isinstance(sec, int) or not PATROL_SEC_MIN <= sec <= PATROL_SEC_MAX:
            return None
        out.append(f"{mode.strip()}:{sec}")
    return ",".join(out)


def clean_hunt_ssid(v):
    """A network name for "huntssid" (C7): 1..32 bytes of UTF-8 (an SSID's limit), no control characters (the
    device stores SSIDs with them replaced, so such a name could never match, and CR/LF would end the serial
    line), and not "0", which the device reads as "stop". Kept exactly as given - no strip: the match is exact,
    and an SSID may start or end with a space. Returns None when it is not a usable name."""
    if not isinstance(v, str) or v == "0":
        return None
    try:
        n = len(v.encode("utf-8"))   # a lone surrogate (legal in JSON) cannot be sent at all
    except UnicodeEncodeError:
        return None
    if not 1 <= n <= 32:
        return None
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in v):
        return None
    return v


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
        self._flushed = time.time()

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
        orig_len = max(orig_len, len(data))   # pcap requires orig_len >= incl_len
        rec = struct.pack("<IIII", sec, usec, len(rt) + len(data), len(rt) + orig_len) + rt + data
        with self.lock:
            if self.f.closed:
                return
            self.f.write(rec)
            self.frames += 1
            self.bytes += len(data)
            if now - self._flushed >= PCAP_FLUSH_S:   # a killed host keeps everything but the last second
                self.f.flush()
                self._flushed = now

    def close(self):
        with self.lock:
            if not self.f.closed:
                self.f.close()


class LineSplitter:
    """Serial bytes -> lines (no trailing CR/LF). A line longer than `limit` with no newline in sight is dropped
    whole - its head now, its tail when the newline finally arrives - so the buffer stays bounded."""

    def __init__(self, limit=SERIAL_LINE_MAX, on_overflow=None):
        self.limit = limit
        self.on_overflow = on_overflow
        self.buf = b""
        self.skipping = False   # discarding an over-long line up to its newline

    def feed(self, chunk):
        self.buf += chunk
        out = []
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if self.skipping:   # the tail of a line already dropped: not a line of its own
                self.skipping = False
                continue
            out.append(line.rstrip(b"\r"))
        if len(self.buf) > self.limit:
            if not self.skipping and self.on_overflow:
                self.on_overflow()
            self.buf = b""
            self.skipping = True
        return out


class Bandwatch:
    def __init__(self, port, captures_dir, oui, fcs_present=True):
        self.port_name = port
        self.captures_dir = captures_dir
        self.fcs_present = fcs_present
        self.oui = oui
        self.ser = None
        self.lock = threading.Lock()     # the serial port (send() and the reader's hand-over); never held while
        # taking dlock, so the order is always dlock -> lock
        # The state lock: the reader thread mutates self.state and the device tables under it, and snapshot()
        # copies everything it returns under it, so an HTTP thread never serializes a dict or deque mid-update.
        # Re-entrant because _dispatch holds it while calling merge_*/stop_capture, which take it themselves.
        self.dlock = threading.RLock()
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
            "sd_rm": None,           # last card-file delete: {name,pending,ok,msg,t} (the dashboard's confirm/toast)
            "events": None,          # C4 SD event log status, the device's "ev" object (None = old firmware)
            "alerts": None,          # LED alert blips on/off (hello + "alerts" ack; None = firmware without it)
            # C5 patrol: None = firmware without it; {"on": False} = idle; {"on": True, "leg", "left", "cyc", "legs",
            # "t"} while it walks (from hello, the {"t":"pt"} line every 2 s and the "patrol" ack). See _set_patrol().
            "patrol": None,
            # completed captures this session, per sink, for the dashboard counters
            "saved": {"usb": {"count": 0, "last": None, "frames": 0, "bytes": 0},
                      "sd":  {"count": 0, "last": None, "frames": 0, "bytes": 0}},
        }
        self.wifi_devs = {}
        self.ble_devs = {}
        self.z_devs = {}
        self.probes = {}         # C1: probing MAC -> {"ssids": {ssid: last_ts}, "rssi", "ch", "last"}
        # Live LCD mirror: the device streams "M x y w h <base64 RGB565-LE>" regions while `mirror` is on and
        # ends every LVGL refresh with an "MF <seq> <complete>" marker. Regions are blitted into a back buffer;
        # `screen` (served at /screen.bin) is only replaced at a marker, so a viewer never sees a half-drawn
        # frame. Firmware older than the markers is detected and served region-by-region as before.
        self.screen_w, self.screen_h = 172, 320
        self.screen = bytearray(self.screen_w * self.screen_h * 2)   # published frame: RGB565-LE, row-major
        self.screen_back = bytearray(self.screen)                     # where regions land between markers
        self.screen_back_dirty = False   # back buffer has regions the published frame lacks
        self.screen_seq = 0              # bumps once per published frame (the dashboard repaints on change)
        self.screen_last = 0.0           # last M/MF line (the mirror is "on" while this is fresh)
        self.screen_pub_t = 0.0          # when `screen` was last published
        self.screen_framed = False       # MF markers seen: publish at markers, not per region
        self.screen_m_since_mf = 0       # regions since the last marker (a long run means no markers any more)
        self.screen_complete = False     # the published frame is a clean, fully-repaired one
        self.screen_stats = {"frames": 0, "complete_frames": 0, "held": 0, "gaps": 0}   # not "complete": that key is
        # the published frame's flag in screen_status(), and **screen_stats there would overwrite it
        self.screen_mf_seq = None
        self.screen_lock = threading.Lock()
        self.cap_band = None
        self.history = deque(maxlen=HISTORY_LEN)
        self._last_hist = 0
        self.spec_hist = {}          # spectrum mode: MHz -> deque of recent edMax (dBm), for the noise floor/duty
        # Sticky explanations: MHz -> {src, label, ts}. A live decode writes/overwrites the entry for that
        # frequency; it then persists (shown as cached, with age) until a newer decode replaces it or the user
        # clears it. Lets the spectrum stay annotated between scans despite the single-radio limitation.
        self.spec_explained = {}
        # "Explain" orchestration: a background thread that leaves spec mode to re-decode Wi-Fi/BLE/15.4 (so the
        # unexplained-energy comparison has fresh "known" devices), then jumps back to spec. See start_explain().
        self._explain_thread = None
        self._explain_lock = threading.Lock()
        self.state["explain"] = None   # {active, scope, leg, legs, phase, until} while running
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
        s.write_timeout = SERIAL_WRITE_TIMEOUT_S   # a stalled USB stream must not hang send() (and self.lock) forever
        # The ESP32-C5's USB-Serial-JTAG turns DTR/RTS edges into BOOT/EN pulses. macOS asserts both lines
        # on open; keeping them asserted (no edge) leaves the board running. Dropping them reboots it.
        s.dtr = True
        s.rts = True
        # Exclusive (flock): a second host on the same port used to open it too, and the two then split the
        # incoming bytes - each saw "readiness to read but returned no data", reconnected every few seconds, and
        # lost lines, which looked exactly like a hung board. Now the second one is refused with a clear message.
        s.exclusive = True
        s.open()
        return s

    def _log(self, msg):
        with self.dlock:
            self.state["log"].append(msg)

    def connected(self):
        return self.ser is not None

    def send(self, cmd, strip=True):
        err = None
        with self.lock:
            if self.ser is None:
                return False
            # Collapse embedded newlines/CRs so a value from the HTTP API (e.g. a crafted "mac") can't
            # smuggle a second command onto the serial line. strip=False keeps edge spaces that are part of the
            # argument (an SSID for "huntssid" may end in one); its caller has already refused control chars.
            line = (cmd.strip() if strip else cmd).replace("\r", " ").replace("\n", " ")
            try:
                self.ser.write((line + "\n").encode())
            except Exception as e:   # includes serial.SerialTimeoutException (write_timeout)
                err = e
        if err is None:
            return True
        self._log(f"write failed: {err}")   # outside self.lock: the lock order is dlock -> lock
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
                busy = "lock" in str(e).lower() or "busy" in str(e).lower() or "resource temporarily unavailable" in str(e).lower()
                msg = (f"open {port}: in use by another program (a second bandwatch_host.py, the device tests, "
                       f"power_profile.py or a serial monitor?) - close it; retrying") if busy else f"open {port}: {e}"
                with self.dlock:
                    if not self.state["log"] or self.state["log"][-1] != msg:   # don't flood the log while retrying
                        self.state["log"].append(msg)
                if busy and not getattr(self, "_busy_warned", False):
                    print(msg, file=sys.stderr)
                    self._busy_warned = True
                time.sleep(1.5)
                continue
            self._busy_warned = False
            with self.lock:
                self.ser = ser
            with self.dlock:
                self.state["connected"] = True
                self.state["port"] = port
                self.state["log"].append(f"connected to {port}")
            self.send(f"time {int(time.time())}")   # device has no RTC: pcap timestamps come from this
            self.send("info")
            lines = LineSplitter(on_overflow=lambda: self._log(f"serial line over {SERIAL_LINE_MAX} bytes dropped"))
            try:
                while True:
                    chunk = ser.read(4096)
                    if not chunk:
                        continue
                    for line in lines.feed(chunk):
                        self.handle_line(line)
            except Exception as e:
                self._log(f"serial error: {e}")
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

    def _set_hunt(self, mac, ssid=None):
        """The device's "hunt" field (hello / hunt ack): a MAC or 15.4 id, or for an SSID hunt (C7) the name, with
        "ssid" alongside - firmware before C7 never sends "ssid". A different target starts a fresh history."""
        st = self.state
        ssid = ssid if isinstance(ssid, str) and ssid else None
        if not isinstance(mac, str):   # a malformed line ({} / [] / 7) must not become a dict key in snapshot()
            mac = None
        if mac and (st["hunt"] is None or st["hunt"]["mac"] != mac or st["hunt"].get("ssid") != ssid):
            st["hunt"] = {"mac": mac, "ssid": ssid, "rssi": None, "age_ms": None, "count": 0,
                          "hist": deque(maxlen=400)}
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

    def _set_events(self, ev):
        """C4 event log status. The device's "ev" object: on (armed), card (last mount attempt worked), base
        (baseline MACs in RAM), file (entries in /seen.csv), written / pending (/events.csv rows written / still
        buffered), surv / new (rows by kind), drop (rows lost), err (card errors), wait (novelty checks skipped
        because no baseline is loaded yet). Absent on firmware before C4: keep whatever we had (None)."""
        if isinstance(ev, dict):
            self.state["events"] = dict(ev)

    def _set_patrol(self, msg):
        """C5 patrol state from a line that carries the "pt" member (hello, {"t":"pt"}, the "patrol" ack): null =
        idle, else {"leg": i, "left": ms left in the leg, "cyc": cycles done, "legs": [[mode, sec], ...]}. A line
        without the member (firmware before C5) leaves the state alone. "t" stamps it, so snapshot() can count
        "left" down between the 2 s status lines."""
        if "pt" not in msg:
            return
        pt = msg.get("pt")
        if not isinstance(pt, dict):
            self.state["patrol"] = {"on": False}
            return
        legs = [[str(l[0]), int(l[1])] for l in pt.get("legs", []) if isinstance(l, list) and len(l) >= 2]
        leg = int(pt.get("leg", 0))
        self.state["patrol"] = {"on": True, "leg": leg, "left": int(pt.get("left", 0)), "cyc": int(pt.get("cyc", 0)),
                                "legs": legs, "t": time.time()}

    def patrolling(self):
        p = self.state.get("patrol")
        return bool(p and p.get("on"))

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
        if raw.startswith(b"M "):
            self.handle_mirror(raw)     # live LCD-mirror region; not JSON
            return
        if raw.startswith(b"MF "):
            self.handle_mirror_frame(raw)   # live LCD-mirror frame marker
            return
        try:
            msg = json.loads(raw.decode("utf-8", "replace"))
        except Exception:
            if raw.strip():
                self._log(raw.decode("utf-8", "replace")[:160])
            return
        if not isinstance(msg, dict):
            # Valid JSON but not an object (boot noise like a bare number): log it. Passing it on would raise
            # in the except clause below (msg.get), out of handle_line, and end the serial session.
            self._log(raw.decode("utf-8", "replace")[:160])
            return
        # A line that parses as JSON but has a field missing or the wrong shape must not escape to
        # reader(): an exception there drops the serial session and silently stops a running capture.
        # The whole dispatch runs under the state lock, so snapshot() sees a line applied entirely or not at all.
        with self.dlock:
            try:
                self._dispatch(msg)
            except Exception as e:
                self.state["log"].append(f"bad {msg.get('t')!r} line ({e.__class__.__name__}: {e})")

    def _dispatch(self, msg):
        t = msg.get("t")
        st = self.state
        if t == "hello":
            if not isinstance(msg.get("chs", []), list):   # before any state changes: snapshot() iterates chs
                raise ValueError("chs is not a list")
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
            self._set_hunt(msg.get("hunt"), msg.get("ssid"))
            self._hunt_update(msg.get("h"))
            self._set_deauth(msg.get("deauth"))
            if msg.get("sd") is not None:
                # hello's sd has no file listing: keep the one we have while the card is still there, so a
                # reconnect or an "info" does not blank the dashboard's file card until the next sdls.
                old = st.get("sd") or {}
                sd = dict(msg["sd"])
                if sd.get("mounted") and old.get("mounted"):
                    for k in ("files", "file_total"):
                        if k in old:
                            sd[k] = old[k]
                st["sd"] = sd
                self._sd_track(st["sd"])
            self._set_events(msg.get("ev"))
            if "alerts" in msg:
                st["alerts"] = 1 if msg.get("alerts") else 0
            self._set_patrol(msg)
        elif t == "ev":
            self._set_events(msg.get("ev"))
        elif t == "pt":
            self._set_patrol(msg)
        elif t == "d":
            c = msg["c"]
            entry = {"s": msg["s"], "r": msg["r"], "f": msg["f"], "b": msg["b"], "st": msg["st"],
                     "u": msg["u"], "state": 0, "t": time.time()}
            if msg.get("top"):   # C6 top talker: strongest transmitter this dwell (null when no frame seen)
                entry["top"] = msg["top"]
                entry["trssi"] = msg.get("trssi")
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
                # Trend history: timestamp, band-wide max (busiest channel), the channel sampled now + its
                # score, and the band AVERAGE over channels sampled in the last ~15 s (≈2 sweeps). The max
                # pins high in busy air; the average moves, giving a sense of overall load vs the one peak.
                fresh = [e["s"] for e in st["channels"].values()
                         if e.get("state") == 0 and now - e.get("t", 0) < 15]
                avg = round(sum(fresh) / len(fresh), 1) if fresh else 0.0
                self.history.append((round(now, 1), msg["g"], c, msg["s"], avg))
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
                if prev.get("top"):   # keep C6 top talker across sweep rows (they carry no per-dwell detail)
                    entry["top"] = prev["top"]
                    entry["trssi"] = prev.get("trssi")
                st["channels"][c] = entry
                self._cache_wide(st["band"], c, entry)   # caches 5 GHz activity for the combined view
            st["chs"] = chs
            st["channels"] = {c: v for c, v in st["channels"].items() if c in chs}
        elif t == "fs":
            # Fine 2.4 GHz spectrum sweep: full set of frequency bins.
            bins = msg.get("bins", [])
            self._spec_step_seen(msg.get("step", 2))
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
            mhz = msg.get("mhz", 0)
            self._spec_step_seen(msg.get("step", self.fine["step"]))
            self.fine["current_mhz"] = mhz
            self.fine["step"] = msg.get("step", self.fine["step"])
            st["spec_step"] = msg.get("step", st.get("spec_step", 2))
            st["sweep"] = msg.get("n", st.get("sweep", 0))
            # Write this one bin live so the dashboard bars track each probe, not only once per full sweep (fs).
            # In-place element swap is GIL-safe against snapshot() iterating the same list; same [min,mean,max,ns]
            # shape as an fs row.
            lo, step = self.fine.get("lo", 2400), self.fine.get("step", 2) or 2
            bins = self.fine.get("bins") or []
            i = (mhz - lo) // step if mhz else -1
            if 0 <= i < len(bins):
                bins[i] = [msg.get("min", 0), msg.get("mean", 0), msg.get("max", 0), msg.get("ns", 0)]
                self.fine["ts"] = time.time()
                if msg.get("ns"):
                    self._spec_track(mhz, msg.get("max", 0))
        elif t == "w":
            self.merge_wifi(msg.get("dev", []))
        elif t == "z":
            self.merge_154(msg.get("dev", []))
        elif t == "pr":
            self.merge_probe(msg)
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
            if msg.get("cmd") == "hunt":   # "huntssid" acks with this shape too, plus "ssid"
                self._set_hunt(msg.get("hunt"), msg.get("ssid"))
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
            if msg.get("cmd") == "events":
                self._set_events(msg.get("ev"))
            if msg.get("cmd") == "alerts" and "alerts" in msg:
                st["alerts"] = 1 if msg.get("alerts") else 0
            if msg.get("cmd") == "patrol":
                self._set_patrol(msg)
            if msg.get("cmd") == "sdread":
                self._sd_read_start(msg.get("file"), msg.get("bytes"))   # the ack precedes the S chunks
            elif msg.get("cmd") == "sdread_done":
                self._sd_read_finish(True, msg.get("sent"))
            elif msg.get("cmd") == "sdrm":
                self._sd_rm_result(msg.get("file"), bool(msg.get("ok")), msg.get("msg"))
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
            text = str(msg.get("msg", ""))
            if t == "log" and text.startswith("sd card "):
                # The device's presence tracking (mounts, I/O failures, an idle CMD0 probe every 2 s): keep the
                # dashboard's idea of the card in step instead of waiting for the next hello/sdinfo.
                sd = st.get("sd") or {}
                sd["mounted"] = 1 if text.startswith("sd card inserted") else 0
                if not sd["mounted"]:
                    sd.pop("files", None)   # the listing belonged to the card that just left
                st["sd"] = sd
            if text.startswith("sdread"):
                self._sd_read_finish(False)   # the file will not be coming; close out the half-built buffer
            elif t == "err" and text.startswith("sdrm:"):
                self._sd_rm_result(None, False, text[5:].strip())   # refused (busy, no card, bad name)
            elif t == "err" and str(msg.get("msg", "")).startswith("sdcap:") and "recording stopped" in str(msg.get("msg")):
                # A card write failed (pulled mid-recording): the device has already stopped. Clear cap now
                # rather than wait for the next d/ble line's "sdc", and let _sd_track count the partial file.
                sd = st.get("sd") or {}
                sd["cap"] = 0
                st["sd"] = sd
                self._sd_track(sd)

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
                d["vendor"] = self.oui.lookup(mac)
                # tier 2 = we heard it transmit; tier 1 = only ever seen as a frame destination (addr1)
                dest_only = bool(flags & 4)
                # rssi/max are null for dest-only: the device has none for them (its 0, or a sender's RSSI leaked
                # into maxRssi), and the dashboard shows "-" with no bar.
                d.update({"rssi": None if dest_only else rssi, "max": None if dest_only else mx,
                          "frames": frames, "last": now - age / 1000.0, "ch": ch,
                          "ap": bool(flags & 1), "ssid": ssid or d["ssid"],
                          "dest_only": dest_only, "ap_suffix": ap_suffix or d.get("ap_suffix", ""),
                          "surv": SURV_CAT.get(surv, ""), "surv_kind": SURV_KIND.get(surv, ""),
                          "tier": (0 if not surv else 1 if dest_only else 2)})
                sec, pmf, phy, bw, util, stations, cc = extra
                if flags & 2:
                    d.update({"sec": sec_string(sec, pmf), "phy": phy_string(phy, ch), "bw": bw * 10 if bw else None,
                              "util": round(util * 100 / 255) if util else None, "stations": stations, "cc": cc})
                # Insert only once the row parsed: a malformed row must not leave a half-built entry (no "last")
                # that makes every later _expire()/snapshot() raise.
                self._make_room(self.wifi_devs, mac)
                self.wifi_devs[mac] = d
                # A dest-only (tier-1) row carries no signal of its own (the device never writes its rssi), so
                # no RSSI history point either: the sparkline would dip to a fake 0 dBm.
                if not dest_only and age < 4000 and (not d["hist"] or now - d["hist"][-1][0] >= 1.5):
                    d["hist"].append((round(now, 1), rssi))
            self._expire(self.wifi_devs, now)
            self._resolve_parents()

    def merge_probe(self, msg):
        """C1: one directed probe request - a client naming a network it wants. The device dedups each (MAC, SSID)
        pair for 60 s, so this sees at most ~1/min per pair; history and expiry live here."""
        mac, ssid = msg.get("mac"), msg.get("ssid")
        if not mac or not ssid:
            return
        now = time.time()
        with self.dlock:
            p = self.probes.get(mac)
            if p is None:
                if len(self.probes) >= PROBE_MAX:   # drop the stalest probing MAC
                    del self.probes[min(self.probes, key=lambda m: self.probes[m]["last"])]
                p = self.probes[mac] = {"ssids": {}}
            p["ssids"][ssid] = now
            if len(p["ssids"]) > PROBE_SSIDS_MAX:   # one MAC naming endless SSIDs: keep the most recent
                del p["ssids"][min(p["ssids"], key=p["ssids"].get)]
            p.update({"rssi": msg.get("rssi"), "ch": msg.get("ch"), "last": now})

    @staticmethod
    def _random_mac(mac):
        """Locally administered (bit 1 of the first octet): almost always a privacy-randomized client MAC."""
        try:
            return bool(int(mac[:2], 16) & 0x02)
        except ValueError:
            return False

    def _probe_view(self, now):
        """Expire stale pairings, then group by SSID: a randomizing phone shows up as many MACs asking for one
        network, so the SSID is the stable key. Caller holds self.dlock."""
        for mac in list(self.probes):
            p = self.probes[mac]
            p["ssids"] = {s: t for s, t in p["ssids"].items() if now - t <= PROBE_TTL_S}
            if not p["ssids"]:
                del self.probes[mac]
        ap_ssids = {d.get("ssid") for d in self.wifi_devs.values() if d.get("ap") and d.get("ssid")}
        groups = {}
        for mac, p in self.probes.items():
            for ssid, t in p["ssids"].items():
                g = groups.setdefault(ssid, {"ssid": ssid, "macs": 0, "random": 0, "last": 0, "rssi": -127,
                                             "nearby_ap": ssid in ap_ssids, "sample": []})
                g["macs"] += 1
                g["random"] += self._random_mac(mac)
                g["last"] = max(g["last"], t)
                g["rssi"] = max(g["rssi"], p.get("rssi") if p.get("rssi") is not None else -127)
                if len(g["sample"]) < 4 and not self._random_mac(mac):
                    g["sample"].append({"mac": mac, "vendor": self.oui.lookup(mac)})
        out = sorted(groups.values(), key=lambda g: -g["last"])
        for g in out:
            g["age"] = round(now - g["last"], 1)
            del g["last"]
        return out

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
                self._make_room(self.ble_devs, mac)
                self.ble_devs[mac] = d   # only once the row parsed (see merge_wifi)
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
                ext = bool(flags & 1)
                d.update({"rssi": rssi, "max": mx, "frames": frames, "last": now - age / 1000.0, "ch": ch,
                          "pan": None if pan == 0xFFFF else f"{pan:04x}", "short": None if short == 0xFFFF else f"{short:04x}",
                          "ext": key if ext else "", "vendor": self.oui.lookup(key) if ext else "",
                          "proto": PROTO_154.get(proto, "?"), "proto_id": proto,
                          "beacons": bool(flags & 2), "permit_join": bool(flags & 4), "mac_secured": bool(flags & 8),
                          "data": bool(flags & 16), "lqi": lqi})
                self._make_room(self.z_devs, key)
                self.z_devs[key] = d     # only once the row parsed (see merge_wifi)
                if age < 4000 and (not d["hist"] or now - d["hist"][-1][0] >= 1.5):
                    d["hist"].append((round(now, 1), rssi))
            self._expire(self.z_devs, now)

    @staticmethod
    def _expire(table, now):
        for mac in [m for m, d in table.items() if now - d["last"] > DEV_EXPIRE_S]:
            del table[mac]

    @staticmethod
    def _make_room(table, key):
        """Before inserting a new key: at DEV_MAX entries, evict the one heard longest ago."""
        if key not in table and len(table) >= DEV_MAX:
            del table[min(table, key=lambda k: table[k]["last"])]

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

    def _spec_step_seen(self, step):
        """A new fine-spectrum step (1/2/5 MHz) puts the bins on other frequencies: drop the per-MHz history, or the
        old keys linger as ghost "unexplained" rows and drag the noise floor. The old bins go too (wrong grid)."""
        with self.dlock:
            if step and step != self.fine.get("step"):
                self.spec_hist.clear()
                self.fine["bins"] = []
                self.fine["step"] = step

    def _spec_track(self, mhz, ed_max):
        with self.dlock:   # read by _unidentified() on HTTP threads; keyed by frequency (MHz)
            h = self.spec_hist.get(mhz)
            if h is None:
                h = deque(maxlen=SPEC_HIST_LEN)
                self.spec_hist[mhz] = h
            h.append(ed_max)

    def _spec_known_spans(self, now):
        """Frequency spans we have actually decoded recently, each tagged with its source and the specific
        device that accounts for it, so an energy bin falling in one is "explained" by that device. The single
        radio can't decode and energy-scan at once, so "known" is drawn from the last time those modes ran.
        Each span: {lo, hi, src, label, str} - str (the device's peak RSSI) breaks ties when spans overlap."""
        spans = []
        with self.dlock:
            for d in self.wifi_devs.values():
                ch = d.get("ch") or 0
                if 1 <= ch <= 14 and now - d["last"] <= SPEC_KNOWN_AGE_S:
                    f = 2412 + 5 * (ch - 1)
                    name = d.get("ssid") or d.get("vendor") or ""
                    label = (name + " · " if name else "") + d["mac"]
                    sig = d.get("max") if d.get("max") is not None else d.get("rssi")   # null for dest-only rows
                    spans.append({"lo": f - 11, "hi": f + 11, "src": "wifi", "label": label,    # ~22 MHz wide
                                  "str": sig if sig is not None else -128})
            for d in self.z_devs.values():
                ch = d.get("ch") or 0
                if 11 <= ch <= 26 and now - d["last"] <= SPEC_KNOWN_AGE_S:
                    f = 2405 + 5 * (ch - 11)
                    pan = d.get("pan")
                    label = (d.get("proto") or "802.15.4") + (f" · pan {pan}" if pan else "") + (f" · {d['short']}" if d.get("short") else "")
                    spans.append({"lo": f - 1, "hi": f + 1, "src": "zigbee", "label": label,
                                  "str": d.get("max", d.get("rssi", -128))})
            bles = [d for d in self.ble_devs.values() if now - d["last"] <= SPEC_KNOWN_AGE_S]
        if bles:
            # BLE advertises on 2402/2426/2480 and hops data channels; attribute the adv frequencies to the
            # strongest BLE device we've heard (a loose attribution - BLE hops - hence "+N more").
            strongest = max(bles, key=lambda d: d.get("max", d.get("rssi", -128)))
            nm = strongest.get("name") or strongest.get("vendor") or strongest.get("mac")
            label = nm + (f" +{len(bles) - 1} more" if len(bles) > 1 else "")
            for f in (2402, 2426, 2480):
                spans.append({"lo": f - 1, "hi": f + 1, "src": "ble", "label": label,
                              "str": strongest.get("max", strongest.get("rssi", -128))})
        return spans

    def clear_explained(self):
        """Forget all sticky explanations (the user's 'clear')."""
        with self.dlock:
            self.spec_explained.clear()

    def _spec_analyze(self, remember=True):
        """Classify every 2.4 GHz energy bin carrying sustained power by the specific device that accounts for
        it (wifi/ble/zigbee), or 'unexplained'. A live decode (device seen < SPEC_KNOWN_AGE_S) writes a sticky
        entry per frequency; between scans a bin falls back to its sticky entry, flagged with its age, until a
        newer decode overwrites it or the user clears it. Returns {unidentified, cls:{mhz:source},
        expl:{mhz:device}, age:{mhz:seconds}} - age 0 = live, >0 = cached. 'unexplained' is evidence, not an id.
        remember=False (outside spec mode: the energy is a cached sweep) classifies read-only - it never writes
        sticky entries, so stale energy cannot extend the remembered explanations."""
        now = time.time()
        spans = self._spec_known_spans(now)
        # Snapshot the per-bin history and the sticky map under the lock (the reader thread appends to the
        # deques), then work off the copies. Noise floor = the band-wide quiet level (a low percentile across
        # all bins and recent sweeps), so a continuously-on emitter is measured against ambient quiet.
        with self.dlock:
            hist = {mhz: list(h) for mhz, h in self.spec_hist.items()}   # keyed by frequency (MHz)
            sticky = dict(self.spec_explained)
        all_samples = sorted(v for h in hist.values() for v in h)
        if len(all_samples) < 8:
            return {"unidentified": [], "cls": {}, "expl": {}, "age": {}}
        floor = all_samples[max(0, int(len(all_samples) * 0.15) - 1)]
        gate = floor + SPEC_FLOOR_MARGIN
        unid, cls, expl, age, new_sticky = [], {}, {}, {}, {}
        for mhz, h in hist.items():
            if not h:
                continue
            peak = max(h)
            duty = sum(1 for v in h if v > gate) / len(h)
            if peak <= gate or duty < SPEC_MIN_DUTY:
                continue   # no sustained energy: leave unclassified (drawn as plain energy, not flagged)
            covering = [s for s in spans if s["lo"] <= mhz <= s["hi"]]
            if covering:
                best = max(covering, key=lambda s: s["str"])   # strongest device covering this frequency, live
                cls[mhz], expl[mhz], age[mhz] = best["src"], best["label"], 0
                new_sticky[mhz] = {"src": best["src"], "label": best["label"], "ts": now}
            elif mhz in sticky:
                s = sticky[mhz]                                 # no live decode: fall back to the cached one
                cls[mhz], expl[mhz], age[mhz] = s["src"], s["label"], round(now - s["ts"])
            else:
                cls[mhz] = "unexplained"
                unid.append({"freq_mhz": mhz, "peak_dbm": peak, "floor_dbm": floor, "duty": round(duty, 2)})
        if new_sticky and remember:
            with self.dlock:
                self.spec_explained.update(new_sticky)
        unid.sort(key=lambda e: e["peak_dbm"], reverse=True)
        return {"unidentified": unid, "cls": cls, "expl": expl, "age": age}

    def _unidentified(self):
        """Just the unexplained-energy list (used by the explain orchestration); see _spec_analyze()."""
        return self._spec_analyze()["unidentified"]

    # ---------------- "explain" orchestration ----------------
    def start_explain(self, scope):
        """Leave spec mode, re-decode Wi-Fi/BLE/15.4 so the unexplained-energy comparison has fresh 'known'
        devices, then jump back to spec. scope 'full' = sweep all three 2.4 GHz modes; 'current' = park only
        on the channels covering the frequencies flagged unexplained right now. Runs in a daemon thread so the
        HTTP server stays responsive; a second request while one runs is ignored."""
        with self._explain_lock:
            if self._explain_thread and self._explain_thread.is_alive():
                return False
            self._explain_thread = threading.Thread(target=self._explain_run, args=(scope,), daemon=True)
            self._explain_thread.start()
            return True

    def _explain_legs_current(self):
        """Map the currently-unexplained frequencies to the decode legs that could account for them: the Wi-Fi
        2.4 GHz channel and/or 15.4 channel covering each bin, plus BLE if an advertising frequency is flagged."""
        wifi_chs, z_chs, need_ble = set(), set(), False
        for e in self._unidentified():
            f = e["freq_mhz"]
            wch = round((f - 2412) / 5) + 1            # 2.4 GHz Wi-Fi occupies ~±11 MHz, so most bins map to one
            if 1 <= wch <= 13:
                wifi_chs.add(wch)
            zch = round((f - 2405) / 5) + 11           # 15.4 channels are 5 MHz apart, ~2 MHz wide
            if 11 <= zch <= 26 and abs(f - (2405 + 5 * (zch - 11))) <= 2:
                z_chs.add(zch)
            if any(abs(f - a) <= 2 for a in (2402, 2426, 2480)):
                need_ble = True
        legs = [("2.4g", ch, EXPLAIN_DUR_PARK, f"Wi-Fi ch{ch}") for ch in sorted(wifi_chs)]
        legs += [("154", ch, EXPLAIN_DUR_PARK, f"802.15.4 ch{ch}") for ch in sorted(z_chs)]
        if need_ble:
            legs.append(("ble", 0, EXPLAIN_DUR_BLE, "BLE"))
        return legs

    def _explain_run(self, scope):
        legs = []
        try:   # everything inside: a throw anywhere must still clear "active", or the dashboard latch never lets go
            if scope == "current":
                legs = self._explain_legs_current()
            else:   # full
                legs = [("2.4g", 0, EXPLAIN_DUR_WIFI_FULL, "Wi-Fi 2.4 GHz (full)"),
                        ("ble", 0, EXPLAIN_DUR_BLE, "BLE (full)"),
                        ("154", 0, EXPLAIN_DUR_154_FULL, "802.15.4 (full)")]
            for i, (band, ch, dur, phase) in enumerate(legs):
                self.state["explain"] = {"active": True, "scope": scope, "leg": i + 1, "legs": len(legs),
                                         "phase": phase, "until": time.time() + dur + EXPLAIN_SETTLE_S}
                self.send(f"band {band}")
                time.sleep(EXPLAIN_SETTLE_S)
                if ch:                       # 0 = sweep (no park); a real channel = targeted park
                    self.send(f"park {ch}")
                time.sleep(dur)
                if ch:
                    self.send("park 0")      # unpark before the next leg / before returning to spec
            # Jump back to the scanning view regardless of scope (and whether any legs ran).
            self.state["explain"] = {"active": True, "scope": scope, "leg": len(legs), "legs": len(legs),
                                     "phase": "back to spectrum", "until": time.time() + EXPLAIN_SETTLE_S}
            self.send("band spec")
            time.sleep(EXPLAIN_SETTLE_S)
        finally:
            self.state["explain"] = {"active": False, "scope": scope, "leg": len(legs), "legs": len(legs),
                                     "phase": "done", "until": 0, "finished": time.time()}

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
        self._sd_read = {"name": name or "bandwatch.pcap", "total": total or 0, "buf": bytearray(), "bad": 0}
        self.state["sd_read"] = {"name": os.path.basename(self._sd_read["name"]), "total": self._sd_read["total"],
                                 "received": 0, "done": False, "failed": False}

    def _sd_read_finish(self, done, sent=None):
        r = getattr(self, "_sd_read", None)
        if not r:
            return
        self._sd_read = None
        st = dict(self.state.get("sd_read") or {})
        got = len(r["buf"])
        st["received"] = got
        if done and (got != r["total"] or r["bad"] or (sent is not None and sent != got)):
            # The device said done, but what arrived is not the file it announced: a chunk was dropped or mangled
            # on the way (the S line's byte count disagrees, or the sum misses the size from the ack). Saving it
            # would hand Wireshark a corrupt file that looks complete.
            self.state["log"].append(f"sd pull incomplete: {os.path.basename(r['name'])} got {got} of "
                                     f"{r['total']} bytes ({r['bad']} bad chunks) - not saved")
            done = False
        st["done"] = done
        st["failed"] = not done   # an error line ended it: the dashboard stops showing a pull in progress
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

    # ---------------- sdrm (delete a card file) ----------------
    def sd_rm_request(self, name):
        """Called by the HTTP handler just before "sdrm /name" goes out: the dashboard shows it as pending."""
        self.state["sd_rm"] = {"name": os.path.basename(name), "pending": True, "ok": None, "msg": "",
                               "t": time.time()}

    def _sd_rm_result(self, path, ok, text):
        """The device's sdrm ack ({"ok":1|0,"msg":..}) or an "sdrm: ..." err line (refused before touching the
        card; it names no file, so it is pinned on the request in flight). A delete drops the file from the
        cached listing at once and re-asks sdls for the card's real state. Pulled copies in the captures dir are
        never touched: deleting on the card is not deleting the local file."""
        cur = self.state.get("sd_rm") or {}
        name = os.path.basename(str(path)) if path else cur.get("name", "")
        self.state["sd_rm"] = {"name": name, "pending": False, "ok": ok, "msg": str(text or ""), "t": time.time()}
        if ok:
            self.state["log"].append(f"sd file deleted on card: {name}")
            with self.dlock:
                sd = self.state.get("sd") or {}
                files = sd.get("files")
                if isinstance(files, list):
                    kept = [f for f in files if os.path.basename(str(f[0])) != name]
                    gone = len(files) - len(kept)
                    if gone:
                        sd["files"] = kept
                        try:
                            sd["file_total"] = max(0, int(sd.get("file_total", len(files))) - gone)
                        except (TypeError, ValueError):
                            sd["file_total"] = len(kept)
                self.state["sd"] = sd
            self.send("sdls")   # the card's own word on what is left
        else:
            self.state["log"].append(f"sd delete failed: {name or '?'}{': ' + str(text) if text else ''}")

    def handle_sd_chunk(self, raw):
        """One 'S <n> <base64>' chunk of an sdread file streaming off the card (device: serviceSdRead)."""
        r = getattr(self, "_sd_read", None)
        if not r:
            return   # chunks without a start ack have nowhere to land
        try:
            parts = raw.split(b" ", 2)
            data = base64.b64decode(parts[2], validate=True)
            n = int(parts[1])
        except Exception:
            return   # unreadable line: not counted; if it was real data the size check at sdread_done catches it
        with self.dlock:   # snapshot() reads the summary on HTTP threads; keep the counter honest
            if len(data) != n:
                r["bad"] += 1   # the line's own byte count disagrees with its payload: the file is damaged
            r["buf"] += data
            if self.state.get("sd_read") is not None:
                self.state["sd_read"]["received"] = len(r["buf"])

    def handle_mirror(self, raw):
        # "M x y w h <base64 RGB565-LE>": one flushed LCD region. Blit it into the framebuffer at (x,y).
        try:
            parts = raw.split(b" ", 5)
            x, y, w, h = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            data = base64.b64decode(parts[5])
        except Exception:
            return
        W, H = self.screen_w, self.screen_h
        # Reject a malformed or out-of-bounds region rather than letting it corrupt the buffer or wrap a row.
        if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > W or y + h > H or len(data) != w * h * 2:
            return
        row_bytes = w * 2
        now = time.time()
        with self.screen_lock:
            back = self.screen_back
            for row in range(h):
                dst = ((y + row) * W + x) * 2
                src = row * row_bytes
                back[dst:dst + row_bytes] = data[src:src + row_bytes]
            self.screen_back_dirty = True
            self.screen_last = now
            self.screen_m_since_mf += 1
            # Framed firmware ends every refresh with MF well within MIRROR_LEGACY_REGIONS regions; a longer run
            # means markers stopped (older firmware flashed while we run), so fall back to per-region publishing.
            if self.screen_framed and self.screen_m_since_mf > MIRROR_LEGACY_REGIONS:
                self.screen_framed = False
            if not self.screen_framed:      # legacy firmware (no MF): publish as each region arrives
                self._publish_screen(now, complete=False)

    def handle_mirror_frame(self, raw):
        # "MF <seq> <complete>": the last region of one LVGL refresh has been sent. complete=1 means nothing in
        # this refresh was dropped and no repair is pending on the device, so the back buffer equals the panel.
        try:
            parts = raw.split()
            seq, complete = int(parts[1]), int(parts[2]) != 0
        except Exception:
            return
        now = time.time()
        with self.screen_lock:
            self.screen_last = now
            self.screen_framed = True
            self.screen_m_since_mf = 0
            st = self.screen_stats
            if self.screen_mf_seq is not None and seq != (self.screen_mf_seq + 1) & 0xFFFF:
                st["gaps"] += 1         # a marker was dropped (TX buffer full); harmless, the next one covers it
            self.screen_mf_seq = seq
            st["frames"] += 1
            # Publish clean frames immediately. A torn one (device still repairing dropped regions) is held
            # back, unless nothing has been published for MIRROR_HOLD_S - a page that changes faster than the
            # link can carry would otherwise freeze; it then shows the best frame available, as before.
            if complete:
                st["complete_frames"] += 1
                self._publish_screen(now, complete=True)
            elif now - self.screen_pub_t >= MIRROR_HOLD_S:
                self._publish_screen(now, complete=False)
            else:
                st["held"] += 1

    def _publish_screen(self, now, complete):
        # Caller holds screen_lock. Copy the back buffer to the served frame; skip the copy (and the seq bump that
        # makes the dashboard refetch) when no region arrived since the last publish.
        if self.screen_back_dirty:
            self.screen[:] = self.screen_back
            self.screen_back_dirty = False
            self.screen_seq += 1
        self.screen_complete = complete
        self.screen_pub_t = now

    def screen_status(self):
        # /api/screen. Also the safety net for a lost final marker (an MF line can be dropped when the TX buffer is
        # full): once the stream has gone quiet, whatever is in the back buffer is the latest the device sent.
        now = time.time()
        with self.screen_lock:
            if self.screen_back_dirty and now - self.screen_last >= MIRROR_STALE_S:
                self._publish_screen(now, complete=False)
            return {"on": (now - self.screen_last) < 3.0, "seq": self.screen_seq,
                    "w": self.screen_w, "h": self.screen_h, "framed": self.screen_framed,
                    "complete": self.screen_complete, **self.screen_stats}

    # ---------------- control ----------------
    @staticmethod
    def parse_snaplen(snaplen):
        """The capture snap length from the API: None (keep the device's), or an int clamped to the device's 32..1600.
        Not a number: ValueError (the handler's 400), raised before any file is opened."""
        if snaplen in (None, "", 0):
            return None
        return max(32, min(1600, int(snaplen)))

    def _capture_path(self, link):
        """A fresh file name: two captures started in the same second get -2, -3... instead of truncating."""
        stem = time.strftime("bandwatch-%s-%%Y%%m%%d-%%H%%M%%S" % ({"154": "802154", "ble": "ble"}.get(link, "wifi")))
        path = os.path.join(self.captures_dir, stem + ".pcap")
        k = 2
        while os.path.exists(path):
            path = os.path.join(self.captures_dir, f"{stem}-{k}.pcap")
            k += 1
        return path

    def start_capture(self, snaplen=None):
        """Open a pcap and switch device capture on. Returns the capture state, or None when no device is connected
        (no file is created then). snaplen is validated before anything is opened (ValueError)."""
        snap = self.parse_snaplen(snaplen)
        with self.dlock:   # check-and-open under the lock: two concurrent starts cannot both open a file
            if self.pcap:
                return self.state["capture"]
            if not self.connected():
                return None
            os.makedirs(self.captures_dir, exist_ok=True)
            band = self.state["band"]
            link = "154" if band == "154" else "ble" if band == "ble" else "wifi"
            path = self._capture_path(link)
            self.pcap = PcapWriter(path, fcs_present=self.fcs_present, link=link)
            self.cap_band = band
            self.state["capture"] = {"file": path, "frames": 0, "bytes": 0, "started": time.time()}
            self.state["log"].append(f"capture started: {path}")
            cap = self.state["capture"]
        if (snap and not self.send(f"snap {snap}")) or not self.send("cap 1"):
            # The port went away between the check and the write: take the empty file back out.
            with self.dlock:
                pcap = self.pcap if self.pcap and self.pcap.path == cap["file"] else None
                if pcap:
                    self.pcap = None
                    self.state["capture"] = None
            if pcap:
                pcap.close()
                try:
                    os.remove(pcap.path)
                except OSError:
                    pass
            return None
        return cap

    def stop_capture(self):
        self.send("cap 0")
        with self.dlock:   # take the writer out under the lock: the reader's finally and an HTTP thread both call this
            pcap, self.pcap = self.pcap, None
            if pcap:
                pcap.close()
                self.state["log"].append(f"capture stopped: {pcap.path} ({pcap.frames} frames)")
                sv = self.state["saved"]["usb"]
                sv["count"] += 1
                sv["last"] = pcap.path
                sv["frames"] = pcap.frames
                sv["bytes"] = pcap.bytes
            self.state["capture"] = None

    def hunt(self, mac, ch=None):
        """Returns send()'s result (False: no device)."""
        if not mac:
            return self.send("hunt 0")
        mac = mac.strip().lower()
        if ch is None:
            with self.dlock:
                d = self.wifi_devs.get(mac) or self.z_devs.get(mac)
                ch = d["ch"] if d else 0
        return self.send(f"hunt {mac} {int(ch or 0)}")

    def hunt_ssid(self, name):
        """C7: hunt a network name (already through clean_hunt_ssid), or stop with None. The device derives the
        park itself from where the strongest matching beacon was heard. Returns send()'s result."""
        if not name:
            return self.send("huntssid 0")
        return self.send(f"huntssid {name}", strip=False)

    def _hunt_ssid_aps(self, name, now):
        """Every AP in the device table beaconing this exact name, loudest first (rssi None last): what the
        dashboard lists under an SSID hunt. Caller holds dlock."""
        aps = [{"mac": d["mac"], "ch": d.get("ch"), "rssi": d.get("rssi"), "age": round(now - d["last"], 1),
                "vendor": d.get("vendor", "")}
               for d in self.wifi_devs.values() if d.get("ap") and d.get("ssid") == name]
        aps.sort(key=lambda a: (a["rssi"] is None, -(a["rssi"] or 0), a["mac"]))
        return aps

    def snapshot(self):
        """Everything /api/state returns, copied under the state lock: the reader thread mutates these dicts and
        deques, and json.dumps on another thread must never see one mid-update."""
        with self.dlock:
            return self._snapshot_locked()

    def _snapshot_locked(self):
        st = self.state
        now = time.time()
        # Analyze the spectrum once per snapshot: the unexplained list and the per-bin {mhz: source} map both
        # come from it, so the bar colours and the table agree. Outside spec mode this runs on the cached sweep
        # history (read-only, see remember) so the dashboards can show the last unexplained count in any mode;
        # it is a few thousand samples at most (<= 84 bins x SPEC_HIST_LEN) and returns early with no history.
        spec_analysis = self._spec_analyze(remember=st["band"] == "spec")
        spec_cls, spec_expl, spec_age = spec_analysis["cls"], spec_analysis["expl"], spec_analysis["age"]
        chans = [dict(ch=c, **st["channels"].get(c, {})) for c in st["chs"]]
        probe_groups = self._probe_view(now)
        wifi = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1),
                     seeking=sorted(self.probes.get(d["mac"], {}).get("ssids", {})))
                for d in self.wifi_devs.values()]
        ble = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1)) for d in self.ble_devs.values()]
        zig = [dict(d, hist=list(d["hist"]), age=round(now - d["last"], 1)) for d in self.z_devs.values()]
        hunt = None
        hu = st["hunt"]
        if hu and hu.get("ssid"):
            # SSID hunt (C7): the device reports one reading (the strongest matching AP heard recently); the AP list
            # comes from the device table we already have. "mac" is the loudest AP heard in the last 60 s, if any.
            aps = self._hunt_ssid_aps(hu["ssid"], now)
            best = next((a for a in aps if a["age"] < 60 and a["rssi"] is not None), None)
            hunt = {"mac": best["mac"] if best else "", "ssid": hu["ssid"], "rssi": hu["rssi"], "age_ms": hu["age_ms"],
                    "count": hu["count"], "hist": list(hu["hist"]), "label": hu["ssid"],
                    "vendor": best["vendor"] if best else "", "kind": "network name", "aps": aps}
        elif hu:
            src =self.wifi_devs.get(hu["mac"]) or self.ble_devs.get(hu["mac"]) or self.z_devs.get(hu["mac"]) or {}
            hunt = {"mac": hu["mac"], "rssi": hu["rssi"], "age_ms": hu["age_ms"], "count": hu["count"],
                    "hist": list(hu["hist"]), "label": src.get("ssid") or src.get("name") or src.get("proto") or "",
                    "vendor": src.get("vendor", ""), "kind": src.get("kind", "") or (src.get("pan") and "PAN " + src["pan"]) or ""}
        fine = self.fine
        live = copy.deepcopy({k: st.get(k) for k in ("hello", "capture", "explain", "deauth", "ble", "sd", "sd_read",
                                                      "sd_rm", "saved", "events", "alerts", "patrol")})
        patrol = live["patrol"]
        if patrol and patrol.get("on"):
            # Count the leg down between status lines (they come every 2 s; the dashboard polls at 1 Hz). Floors at 0:
            # a due leg waits for the next dwell boundary before the hand-off.
            legs = patrol.get("legs") or []
            leg = patrol.get("leg", 0)
            patrol["left_ms"] = max(0, int(patrol.get("left", 0) - (now - patrol.pop("t", now)) * 1000))
            patrol["band"] = legs[leg][0] if 0 <= leg < len(legs) else None
        return {
            "connected": st["connected"], "port": st["port"],
            "age": round(now - st["last_rx"], 1) if st["last_rx"] else None,
            "band": st["band"], "current": st["current"], "global": st["global"], "sweep": st["sweep"], "aps": st["aps"],
            "park": st["park"], "cap": st["cap"], "drop": st["drop"], "heap": st["heap"], "hello": live["hello"],
            "channels": chans, "history": list(self.history), "capture": live["capture"],
            "unidentified": spec_analysis["unidentified"],
            "explain": live["explain"],
            "wide": self._wide(),
            "spec_step": st.get("spec_step", 2),
            "fine": {"step": fine["step"], "lo": fine["lo"], "count": fine["count"],
                     "current_mhz": fine["current_mhz"],
                     "age": round(now - fine["ts"], 1) if fine["ts"] else None,
                     "bins": [{"mhz": fine["lo"] + i * fine["step"],
                               "min": r[0], "mean": r[1], "max": r[2], "n": r[3],
                               "cls": spec_cls.get(fine["lo"] + i * fine["step"]),
                               "expl": spec_expl.get(fine["lo"] + i * fine["step"]),
                               "age": spec_age.get(fine["lo"] + i * fine["step"])}
                              for i, r in enumerate(fine["bins"]) if len(r) >= 4]},
            "captures_dir": os.path.abspath(self.captures_dir), "log": list(st["log"])[-15:],
            "wifi_devs": wifi, "probes": probe_groups, "ble_devs": ble, "z_devs": zig, "hunt": hunt,
            "deauth": live["deauth"], "ble": live["ble"], "sd": live["sd"], "sd_read": live["sd_read"],
            "sd_rm": live["sd_rm"], "saved": live["saved"], "events": live["events"], "alerts": live["alerts"],
            "patrol": patrol,
            "oui_source": self.oui.source,
        }


WILDCARD_BINDS = ("0.0.0.0", "::", "")
FILE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.(?:pcap|csv)$")   # what /file will serve


def host_name(addr):
    """An address as it appears in a Host header: IPv6 literals in brackets, lower-case."""
    addr = (addr or "").strip().lower()
    return "[%s]" % addr if ":" in addr and not addr.startswith("[") else addr


def is_loopback(bind):
    return bind in ("localhost", "::1", "[::1]") or bind.startswith("127.")


def host_allowed(host, binds, port):
    """DNS-rebinding guard: the Host header must name this server (the bind address, localhost, 127.0.0.1 or
    [::1], with our port) - a rebound attacker domain arrives with its own name in Host. A wildcard bind has no
    single name to check against, so any Host passes there (main() warns loudly about that bind)."""
    if any(b in WILDCARD_BINDS for b in binds):
        return True
    if not host:
        return False
    names = {"localhost", "127.0.0.1", "[::1]"} | {host_name(b) for b in binds}
    allowed = {"%s:%d" % (n, port) for n in names}
    if port == 80:
        allowed |= names
    return host.strip().lower() in allowed


class _NotConnected(Exception):
    pass


def make_handler(bw, page_path, bind=None):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def end_headers(self):
            # Every response: the dashboard (and its Deauth button) must not be framed by another site.
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "frame-ancestors 'none'")
            super().end_headers()

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _error(self, code, msg):
            return self._json({"ok": False, "error": msg}, code)

        def _empty(self, code):
            self.send_response(code)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _host_ok(self):
            binds = [self.server.server_address[0]] + ([bind] if bind else [])
            if host_allowed(self.headers.get("Host"), binds, self.server.server_address[1]):
                return True
            self._error(403, "bad host")
            return False

        def do_GET(self):
            if not self._host_ok():
                return
            path = urlparse(self.path).path
            if path == "/api/state":
                return self._json(bw.snapshot())
            if path == "/api/screen":   # live LCD mirror status (polled fast by the dashboard canvas)
                return self._json(bw.screen_status())
            if path == "/screen.bin":   # raw RGB565 (little-endian) framebuffer, 172x320 - the last published frame
                with bw.screen_lock:
                    body = bytes(bw.screen)
                    seq = bw.screen_seq
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Screen-Seq", str(seq))
                self.send_header("X-Screen-W", str(bw.screen_w))
                self.send_header("X-Screen-H", str(bw.screen_h))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path == "/file":   # capture pulled off the card (or over USB) for download
                name = (parse_qs(urlparse(self.path).query).get("name") or [""])[0]
                # A plain basename of a pcap or csv: it cannot escape the captures dir, and the character set
                # keeps it safe inside the quoted Content-Disposition (no quote, no CR/LF).
                fpath = os.path.join(bw.captures_dir, name)
                if not FILE_NAME_RE.match(name) or ".." in name or not os.path.isfile(fpath):
                    return self._empty(404)
                try:
                    f = open(fpath, "rb")
                except OSError:
                    return self._empty(404)
                with f:
                    size = os.fstat(f.fileno()).st_size
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(size))
                    self.end_headers()
                    shutil.copyfileobj(f, self.wfile, 64 * 1024)   # streamed: a large capture is not read into RAM
                return
            if path in ("/classic", "/classic/"):   # classic dashboard removed in v1.19: keep old bookmarks working
                self.send_response(301)
                self.send_header("Location", "/")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            page = page_path if path in ("/", "/index.html", "/v2", "/v2/", "/v2.html") else None
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
            self._empty(404)

        def _read_body(self):
            """The POST body as a dict, or None after an error reply was sent."""
            # CSRF: a cross-site page can only send a "simple" request (text/plain, form) without a preflight,
            # so requiring JSON forces any foreign script through CORS - which this server never grants.
            ctype = (self.headers.get("Content-Type") or "").split(";", 1)[0].strip().lower()
            if ctype != "application/json":
                self._error(415, "Content-Type must be application/json")
                return None
            origin = self.headers.get("Origin")
            if origin is not None:
                o = urlparse(origin)
                if o.scheme not in ("http", "https") or o.netloc.lower() != (self.headers.get("Host") or "").strip().lower():
                    self._error(403, "foreign origin")
                    return None
            raw_len = (self.headers.get("Content-Length") or "0").strip()
            if not raw_len.isdigit():   # also rejects a negative length, which would block on read(-1)
                self._error(400, "bad Content-Length")
                return None
            n = int(raw_len)
            if n > HTTP_BODY_MAX:
                self._error(413, "body too large")
                return None
            try:
                req = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                self._error(400, "bad json")
                return None
            if not isinstance(req, dict):
                self._error(400, "body must be a JSON object")
                return None
            return req

        def do_POST(self):
            if not self._host_ok():
                return
            path = urlparse(self.path).path
            if path != "/api/cmd":
                return self._error(404, "not found")
            req = self._read_body()
            if req is None:
                return
            try:
                self._command(req)
            except _NotConnected:
                return self._error(503, "not connected")
            except _Reply as r:
                return self._error(r.code, r.msg)
            except (TypeError, ValueError) as e:
                return self._error(400, f"bad argument: {e}")
            return self._json({"ok": True})

        def _command(self, req):
            """Validate one /api/cmd request and act on it. Raises _Reply for a refusal, _NotConnected when it
            needs the device and there is none (checked before anything is changed, so no empty pcap)."""
            def dev(line):
                if not bw.send(line):
                    raise _NotConnected

            def need_device():
                if not bw.connected():
                    raise _NotConnected

            cmd = req.get("cmd")

            def not_while_patrolling(starting):
                # C5 v1: the device refuses these mid-patrol ("<cmd>: stop patrol first"); refusing here too keeps
                # the click from looking accepted (and keeps "capture" from opening a pcap that never grows).
                if starting and bw.patrolling():
                    raise _Reply(409, f"{cmd}: stop patrol first")

            if cmd in ("capture", "sdcap"):
                not_while_patrolling(bool(req.get("value")))
            elif cmd in ("hunt", "deauth"):
                not_while_patrolling(bool(req.get("mac")))
            elif cmd == "dca":
                not_while_patrolling(bool(req.get("client_mac") or req.get("mac") or req.get("ap_bssid")))
            elif cmd == "explain":
                not_while_patrolling(req.get("value") in ("full", "current"))   # its band steps would end the patrol

            if cmd == "patrol":
                # {"value": true|false} = default legs / stop; {"legs": [[mode, sec], ...] or "mode:sec,..."} = custom.
                legs = req.get("legs")
                value = req.get("value")
                if legs is not None:
                    arg = clean_patrol_legs(legs)
                    if not arg:
                        raise _Reply(400, "patrol legs: 2-6 of [mode, sec], mode 5g|2.4g|both|ble|154|spec, 5-600 s")
                elif value in (True, 1, "1", "on"):
                    arg = "1"
                elif value in (False, 0, "0", "off", None):
                    arg = "0"
                else:
                    raise _Reply(400, "patrol value must be on/off")
                if arg != "0":
                    need_device()
                    with bw.dlock:
                        st = bw.state
                        busy = ("capture" if st.get("capture") or st.get("cap") or (st.get("sd") or {}).get("cap")
                                else "hunt" if st.get("hunt") else "deauth" if st.get("deauth") else None)
                    if busy:
                        raise _Reply(409, f"patrol: stop {busy} first")
                dev(f"patrol {arg}")
            elif cmd == "band" and req.get("value") in ("5g", "2.4g", "both", "ble", "154", "spec"):
                dev(f"band {req['value']}")
            elif cmd == "specstep" and int(req.get("value") or 0) in (1, 2, 5):
                dev(f"specstep {int(req['value'])}")
            elif cmd == "park":
                dev(f"park {int(req.get('value') or 0)}")
            elif cmd == "capture":
                if req.get("value"):
                    if bw.start_capture(req.get("snaplen")) is None:   # snaplen checked first (ValueError: 400)
                        raise _NotConnected
                else:
                    bw.stop_capture()   # always closes a host-side file, but say so when the device is gone
                    need_device()
            elif cmd == "hunt":
                target = clean_hunt_id(req.get("mac"))
                if req.get("mac") and not target:
                    raise _Reply(400, "bad hunt target")
                if not bw.hunt(target, req.get("ch")):
                    raise _NotConnected
            elif cmd == "huntssid":   # C7: {"cmd":"huntssid","ssid":"<name>"}; null/"" stops the hunt
                raw = req.get("ssid")
                name = clean_hunt_ssid(raw)
                if raw not in (None, "") and not name:
                    raise _Reply(400, "bad ssid: 1..32 bytes, no control characters, not \"0\"")
                if not bw.hunt_ssid(name):
                    raise _NotConnected
            elif cmd == "deauth":
                mac = clean_mac(req.get("mac"))
                if req.get("mac") and not mac:
                    raise _Reply(400, "bad mac")
                dev(f"deauth {mac or '0'}")   # the device finds the AP's channel itself
            elif cmd == "dca":  # targeted deauth to one specific client
                mac = clean_mac(req.get("client_mac") or req.get("mac"))
                ap_bssid = clean_mac(req.get("ap_bssid"))
                if (req.get("client_mac") or req.get("mac") or req.get("ap_bssid")) and not (mac and ap_bssid):
                    raise _Reply(400, "dca needs a valid client_mac and ap_bssid")
                dev(f"dca {mac} {ap_bssid}" if mac and ap_bssid else "dca 0")
            elif cmd == "sdcap":
                dev(f"sdcap {1 if req.get('value') else 0}")
            elif cmd == "events":   # C4 SD event log; persists on the device
                dev(f"events {1 if req.get('value') else 0}")
            elif cmd in ("sdread", "sdrm"):
                # sdread pulls a card file to the captures dir; sdrm deletes it on the card (the pulled copy
                # here stays). Same guard for both: card root, our own file names, <= 39 chars.
                name = card_file_name(req.get("path") or "")
                if not name:
                    raise _Reply(400, "bad card file path")
                need_device()   # before sd_rm_request: no "pending" delete for a device that is not there
                if cmd == "sdrm":
                    bw.sd_rm_request(name)
                dev(f"{cmd} /{name}")
            elif cmd == "alerts":   # LED alert blips; persists on the device
                dev(f"alerts {1 if req.get('value') else 0}")
            elif cmd == "ledtest" and req.get("value") in ("surv", "new", "join"):
                dev(f"ledtest {req['value']}")
            elif cmd == "mirror":
                dev(f"mirror {1 if req.get('value') else 0}")
            elif cmd == "page" and req.get("value") in ("next", "prev"):
                dev(f"page {req['value']}")   # step the LCD like a BOOT tap
            elif cmd == "addr1":
                dev(f"addr1 {1 if req.get('value') else 0}")
            elif cmd == "blescan" and req.get("value") in ("passive", "active", "auto"):
                dev(f"blescan {req['value']}")
            elif cmd in ("sdinfo", "sdls", "info"):
                dev(cmd)
            elif cmd == "explain" and req.get("value") in ("full", "current"):
                # Host-orchestrated (not a device command): decode the relevant modes, then return to spec.
                need_device()
                if not bw.start_explain(req["value"]):
                    raise _Reply(409, "explain already running")
            elif cmd == "explain" and req.get("value") == "clear":
                bw.clear_explained()   # forget sticky explanations (host-side only: works with no device)
            else:
                raise _Reply(400, "unknown command")

    return Handler


class _Reply(Exception):
    """A refused /api/cmd: the HTTP status and the error text for {"ok": false, "error": ...}."""

    def __init__(self, code, msg):
        super().__init__(msg)
        self.code, self.msg = code, msg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--http", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--ui", metavar="IGNORED",
                    help="deprecated, ignored: the classic dashboard was removed in v1.19 (kept so old scripts still run)")
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
    if args.ui is not None:
        print(f"Bandwatch host: --ui {args.ui} ignored (the classic dashboard was removed in v1.19)")
    srv = ThreadingHTTPServer((args.bind, args.http),
                              make_handler(bw, os.path.join(HERE, "dashboard2.html"), bind=args.bind))
    print(f"Bandwatch host: dashboard at http://{host_name(args.bind)}:{args.http}/  "
          f"(serial: {args.port or 'auto'}, captures: {os.path.abspath(args.captures)}, vendors: {oui.source})")
    if not is_loopback(args.bind):
        bar = "!" * 78
        print(f"{bar}\nWARNING: --bind {args.bind} serves the dashboard beyond this machine, with NO authentication.\n"
              f"Anyone who can reach port {args.http} can start a deauth, delete card files and read captures.\n"
              f"Use the default --bind 127.0.0.1 (or an SSH tunnel) unless you trust every host on that network.\n"
              f"{bar}", file=sys.stderr)

    def on_sigterm(signum, frame):
        raise KeyboardInterrupt   # same clean exit as Ctrl-C: the running pcap is closed, not cut mid-buffer

    signal.signal(signal.SIGTERM, on_sigterm)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bw.stop_capture()


if __name__ == "__main__":
    main()
