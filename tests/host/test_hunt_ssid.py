"""Offline tests for C7 hunt by SSID: the /api/cmd "huntssid" route and its validation, and the host's parsing of
the hunt ack / hello when they carry "ssid" (plus the AP list the snapshot builds from the device table)."""
import copy
import json
import shutil
import unittest

from _support import bh, make_bw, feed, HELLO_24, wifi_row
from test_http import Server


class CleanHuntSsidTest(unittest.TestCase):
    def test_accepts_names_exactly_as_given(self):
        for name in ("HomeNet", "a", "x" * 32, " lead", "trail ", "two  spaces", "Café-5G", "é" * 16,
                     'quo"te\\back', "00", "0 "):
            with self.subTest(name=name):
                self.assertEqual(bh.clean_hunt_ssid(name), name)

    def test_rejects_unusable_names(self):
        for v in (None, "", "0", "x" * 33, "é" * 17, "a\nb", "a\rb", "tab\there", "nul\x00", "del\x7f",
                  "\ud800", 7, ["a"], {"a": 1}):
            with self.subTest(v=v):
                self.assertIsNone(bh.clean_hunt_ssid(v))


class HuntSsidRouteTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        feed(self.bw, HELLO_24)
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)

    def sent_for(self, body):
        self.bw.ser.clear()
        code, _ = self.s.post(body)
        return code, self.bw.ser.lines()

    def test_good(self):
        cases = [
            ({"cmd": "huntssid", "ssid": "HomeNet"}, ["huntssid HomeNet"]),
            ({"cmd": "huntssid", "ssid": "My Net 5G"}, ["huntssid My Net 5G"]),
            ({"cmd": "huntssid", "ssid": "trailing "}, ["huntssid trailing "]),   # edge spaces survive send()
            ({"cmd": "huntssid", "ssid": "x" * 32}, ["huntssid " + "x" * 32]),
            ({"cmd": "huntssid", "ssid": "Café"}, ["huntssid Café"]),
            ({"cmd": "huntssid"}, ["huntssid 0"]),
            ({"cmd": "huntssid", "ssid": None}, ["huntssid 0"]),
            ({"cmd": "huntssid", "ssid": ""}, ["huntssid 0"]),
        ]
        for body, want in cases:
            with self.subTest(body=body):
                self.assertEqual(self.sent_for(body), (200, want))

    def test_line_fits_the_device_buffer(self):
        # pollSerial keeps 63 bytes since C5 (47 before): "huntssid " + 32 bytes of UTF-8 is 41, inside either.
        code, sent = self.sent_for({"cmd": "huntssid", "ssid": "é" * 16})
        self.assertEqual(code, 200)
        self.assertLessEqual(len(sent[0].encode("utf-8")), 47)

    def test_bad_names_are_400_and_send_nothing(self):
        for ssid in ("x" * 33, "é" * 17, "a\nreboot", "a\rb", "tab\t", "\x00", "0", 7, ["a"], {"a": 1}, True):
            with self.subTest(ssid=ssid):
                self.assertEqual(self.sent_for({"cmd": "huntssid", "ssid": ssid}), (400, []))

    def test_not_connected(self):
        bw = make_bw(with_serial=False)
        s = Server(bw)
        self.addCleanup(s.close)
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        self.assertEqual(s.post({"cmd": "huntssid", "ssid": "HomeNet"})[0], 503)


class HuntSsidParseTest(unittest.TestCase):
    def test_ack_starts_ssid_hunt_and_lists_matching_aps(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, {"t": "w", "dev": [
            wifi_row("aa:aa:aa:aa:aa:01", rssi=-70, ch=1, flags=1, ssid="Mesh"),
            wifi_row("aa:aa:aa:aa:aa:02", rssi=-45, ch=36, flags=1, ssid="Mesh"),
            wifi_row("aa:aa:aa:aa:aa:03", rssi=-30, ch=6, flags=1, ssid="mesh"),      # case differs: not a match
            wifi_row("aa:aa:aa:aa:aa:04", rssi=-40, ch=6, flags=0, ssid="Mesh"),      # not an AP
        ]})
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "Mesh", "ssid": "Mesh", "park": 36})
        feed(bw, {"t": "d", "c": 36, "s": 1.0, "r": 1.0, "f": 1, "b": 1, "st": 0, "u": 1, "g": 1.0, "n": 1, "park": 36,
                  "cap": 0, "drop": 0, "da": 0, "df": 0, "sdc": 0, "sdf": 0, "sdb": 0, "top": None, "trssi": None,
                  "h": [-45, 80, 12]})
        snap = bw.snapshot()
        h = snap["hunt"]
        self.assertEqual(h["ssid"], "Mesh")
        self.assertEqual(h["label"], "Mesh")
        self.assertEqual((h["rssi"], h["age_ms"], h["count"]), (-45, 80, 12))
        self.assertEqual(h["mac"], "aa:aa:aa:aa:aa:02")                       # loudest matching AP
        self.assertEqual([a["mac"] for a in h["aps"]], ["aa:aa:aa:aa:aa:02", "aa:aa:aa:aa:aa:01"])
        self.assertEqual([a["ch"] for a in h["aps"]], [36, 1])
        self.assertEqual(snap["park"], 36)
        json.dumps(snap)                                                      # /api/state must serialize

    def test_escaped_name_round_trips(self):
        bw = make_bw()
        name = 'a"b\\c'
        line = '{"t":"ack","cmd":"hunt","hunt":%s,"ssid":%s,"park":0}' % (json.dumps(name), json.dumps(name))
        feed(bw, line)
        self.assertEqual(bw.snapshot()["hunt"]["ssid"], name)

    def test_no_matching_ap_is_an_empty_list(self):
        bw = make_bw()
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "Nowhere", "ssid": "Nowhere", "park": 0})
        h = bw.snapshot()["hunt"]
        self.assertEqual((h["aps"], h["mac"], h["rssi"], h["count"]), ([], "", None, 0))

    def test_hello_carries_ssid_hunt(self):
        bw = make_bw()
        feed(bw, dict(copy.deepcopy(HELLO_24), hunt="Mesh", ssid="Mesh", h=[-60, 300, 4]))
        h = bw.snapshot()["hunt"]
        self.assertEqual((h["ssid"], h["rssi"], h["count"]), ("Mesh", -60, 4))

    def test_mac_hunt_replaces_ssid_hunt_and_back(self):
        bw = make_bw()
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "Mesh", "ssid": "Mesh", "park": 0})
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "aa:bb:cc:dd:ee:ff", "park": 0})
        h = bw.snapshot()["hunt"]
        self.assertEqual(h["mac"], "aa:bb:cc:dd:ee:ff")
        self.assertNotIn("ssid", h)
        # an SSID that happens to equal the old MAC string is still a different hunt (fresh history)
        bw.state["hunt"]["hist"].append((1.0, -50))
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "aa:bb:cc:dd:ee:ff", "ssid": "aa:bb:cc:dd:ee:ff", "park": 0})
        self.assertEqual(bw.snapshot()["hunt"]["ssid"], "aa:bb:cc:dd:ee:ff")
        self.assertEqual(bw.snapshot()["hunt"]["hist"], [])
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": None, "park": 0})
        self.assertIsNone(bw.snapshot()["hunt"])

    def test_old_firmware_ack_without_ssid_is_a_mac_hunt(self):
        bw = make_bw()
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "aa:bb:cc:dd:ee:ff", "park": 6})
        self.assertNotIn("ssid", bw.snapshot()["hunt"])

    def test_malformed_hunt_fields_do_not_break_snapshot(self):
        bw = make_bw()
        for raw in ('{"t":"ack","cmd":"hunt","hunt":{},"ssid":"x"}', '{"t":"ack","cmd":"hunt","hunt":"x","ssid":["a"]}',
                    '{"t":"ack","cmd":"hunt","hunt":[1],"park":0}', '{"t":"ack","cmd":"hunt","hunt":"x","ssid":7}'):
            with self.subTest(raw=raw):
                feed(bw, raw)
                json.dumps(bw.snapshot())


if __name__ == "__main__":
    unittest.main()
