"""Offline tests: the real HTTP handler (make_handler) on an ephemeral port, with a host object whose serial
port is a FakeSerial. Checks page routing, the JSON endpoints, and that every /api/cmd maps to the right
device command string (and that bad input never reaches the device)."""
import json
import os
import shutil
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest import mock

from _support import bh, make_bw, feed, HELLO_24, HOST_DIR

V2 = os.path.join(HOST_DIR, "dashboard2.html")


def read(path):
    with open(path, "rb") as f:
        return f.read()


class Server:
    def __init__(self, bw, page=V2):
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), bh.make_handler(bw, page))
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()

    def get(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=5) as r:
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def get_noredirect(self, path):
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None
        opener = urllib.request.build_opener(NoRedirect)
        try:
            with opener.open(self.base + path, timeout=5) as r:
                return r.status, dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers)

    def post(self, body, path="/api/cmd", raw=None):
        data = raw if raw is not None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()


class RoutingTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)

    def test_dashboard_served_at_root_and_v2(self):
        for path in ("/", "/index.html", "/v2", "/v2/", "/v2.html"):
            with self.subTest(path=path):
                code, body, hdrs = self.s.get(path)
                self.assertEqual(code, 200)
                self.assertEqual(body, read(V2))
                self.assertTrue(hdrs["Content-Type"].startswith("text/html"))

    def test_classic_redirects_to_root(self):
        # The classic dashboard was removed in v1.19; old bookmarks get a permanent redirect to "/".
        for path in ("/classic", "/classic/"):
            with self.subTest(path=path):
                code, hdrs = self.s.get_noredirect(path)
                self.assertEqual(code, 301)
                self.assertEqual(hdrs["Location"], "/")
        code, body, _ = self.s.get("/classic")   # followed: lands on the dashboard
        self.assertEqual((code, body), (200, read(V2)))

    def test_classic_file_is_gone(self):
        self.assertFalse(os.path.exists(os.path.join(HOST_DIR, "dashboard.html")))
        self.assertEqual(self.s.get("/dashboard.html")[0], 404)

    def test_unknown_path_404(self):
        self.assertEqual(self.s.get("/nope")[0], 404)
        self.assertEqual(self.s.get("/api/nope")[0], 404)

    def test_api_state_json(self):
        feed(self.bw, HELLO_24)
        code, body, hdrs = self.s.get("/api/state")
        self.assertEqual(code, 200)
        self.assertEqual(hdrs["Content-Type"], "application/json")
        st = json.loads(body)
        for key in ("connected", "band", "channels", "wifi_devs", "ble_devs", "z_devs", "probes", "sd", "events",
                    "fine", "history", "log", "saved"):
            self.assertIn(key, st)
        self.assertEqual(st["band"], "2.4g")

    def test_api_screen_and_screen_bin(self):
        code, body, _ = self.s.get("/api/screen")
        st = json.loads(body)
        self.assertEqual((st["w"], st["h"]), (172, 320))
        self.assertIsInstance(st["complete"], bool)
        code, body, hdrs = self.s.get("/screen.bin")
        self.assertEqual(code, 200)
        self.assertEqual(len(body), 172 * 320 * 2)
        self.assertEqual(hdrs["X-Screen-Seq"], "0")

    def test_file_download_and_traversal(self):
        with open(os.path.join(self.bw.captures_dir, "events.csv"), "wb") as f:
            f.write(b"x,y\n")
        code, body, hdrs = self.s.get("/file?name=events.csv")
        self.assertEqual((code, body), (200, b"x,y\n"))
        outside = tempfile.NamedTemporaryFile(dir=os.path.dirname(self.bw.captures_dir), delete=False)
        self.addCleanup(os.unlink, outside.name)
        for name in ("", "missing.pcap", "../" + os.path.basename(outside.name), "..%2F" + os.path.basename(outside.name),
                     "/etc/passwd", "%2Fetc%2Fpasswd"):
            with self.subTest(name=name):
                self.assertEqual(self.s.get("/file?name=" + name)[0], 404)


class MissingPageTest(unittest.TestCase):
    def test_missing_dashboard_404s(self):
        s = Server(make_bw(), page=os.path.join(HOST_DIR, "no-such-dashboard.html"))
        self.addCleanup(s.close)
        self.assertEqual(s.get("/")[0], 404)
        self.assertEqual(s.get("/v2")[0], 404)


class UiFlagTest(unittest.TestCase):
    def test_ui_flag_accepted_and_ignored(self):
        # --ui was removed with the classic dashboard (v1.19) but still parses, so old scripts keep running.
        started = {}

        class Stop(Exception):
            pass

        def fake_server(addr, handler):
            started["handler"] = handler
            raise Stop

        for argv in (["--ui", "classic"], ["--ui", "v2"], []):
            with self.subTest(argv=argv), \
                    mock.patch("sys.argv", ["bandwatch_host.py", "--no-oui-download", "--port", "/dev/null-x",
                                               "--captures", tempfile.gettempdir()] + argv), \
                    mock.patch.object(bh, "ThreadingHTTPServer", fake_server), \
                    mock.patch.object(bh.threading, "Thread"), \
                    mock.patch.object(bh.OuiDb, "load_cache", return_value=True), \
                    mock.patch("builtins.print"):
                with self.assertRaises(Stop):
                    bh.main()
                self.assertIn("handler", started)


class CommandMappingTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        feed(self.bw, HELLO_24)
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)

    def sent_for(self, body):
        self.bw.ser.clear()
        code, resp = self.s.post(body)
        return code, self.bw.ser.lines()

    def test_good_commands(self):
        cases = [
            ({"cmd": "band", "value": v}, ["band " + v]) for v in ("5g", "2.4g", "both", "ble", "154", "spec")
        ] + [
            ({"cmd": "specstep", "value": 5}, ["specstep 5"]),
            ({"cmd": "specstep", "value": "1"}, ["specstep 1"]),
            ({"cmd": "park", "value": 36}, ["park 36"]),
            ({"cmd": "park", "value": None}, ["park 0"]),
            ({"cmd": "park"}, ["park 0"]),
            ({"cmd": "hunt", "mac": "AA:BB:CC:DD:EE:FF"}, ["hunt aa:bb:cc:dd:ee:ff 0"]),
            ({"cmd": "hunt", "mac": "aa:bb:cc:dd:ee:ff", "ch": 11}, ["hunt aa:bb:cc:dd:ee:ff 11"]),
            ({"cmd": "hunt", "mac": "1a62/0001"}, ["hunt 1a62/0001 0"]),
            ({"cmd": "hunt", "mac": "00:12:4b:00:01:02:03:04", "ch": 15}, ["hunt 00:12:4b:00:01:02:03:04 15"]),
            ({"cmd": "hunt"}, ["hunt 0"]),
            ({"cmd": "deauth", "mac": "11:22:33:44:55:66"}, ["deauth 11:22:33:44:55:66"]),
            ({"cmd": "deauth"}, ["deauth 0"]),
            ({"cmd": "dca", "client_mac": "aa:aa:aa:aa:aa:aa", "ap_bssid": "11:22:33:44:55:66"},
             ["dca aa:aa:aa:aa:aa:aa 11:22:33:44:55:66"]),
            ({"cmd": "dca", "mac": "aa:aa:aa:aa:aa:aa", "ap_bssid": "11:22:33:44:55:66"},
             ["dca aa:aa:aa:aa:aa:aa 11:22:33:44:55:66"]),
            ({"cmd": "dca"}, ["dca 0"]),
            ({"cmd": "blekick", "mac": "aa:bb:cc:dd:ee:ff"}, ["blekick aa:bb:cc:dd:ee:ff"]),
            ({"cmd": "blekick", "mac": "AA:BB:CC:DD:EE:FF"}, ["blekick aa:bb:cc:dd:ee:ff"]),   # lower-cased, like the rest
            ({"cmd": "blekick"}, ["blekick 0"]),
            ({"cmd": "sdcap", "value": 1}, ["sdcap 1"]),
            ({"cmd": "sdcap", "value": 0}, ["sdcap 0"]),
            ({"cmd": "events", "value": True}, ["events 1"]),
            ({"cmd": "events", "value": False}, ["events 0"]),
            ({"cmd": "alerts", "value": True}, ["alerts 1"]),
            ({"cmd": "alerts", "value": 0}, ["alerts 0"]),
            ({"cmd": "ledtest", "value": "surv"}, ["ledtest surv"]),
            ({"cmd": "ledtest", "value": "join"}, ["ledtest join"]),
            ({"cmd": "mirror", "value": 1}, ["mirror 1"]),
            ({"cmd": "mirror", "value": 0}, ["mirror 0"]),
            ({"cmd": "page", "value": "next"}, ["page next"]),
            ({"cmd": "page", "value": "prev"}, ["page prev"]),
            ({"cmd": "addr1", "value": 1}, ["addr1 1"]),
            ({"cmd": "addr1", "value": 0}, ["addr1 0"]),
            ({"cmd": "blescan", "value": "auto"}, ["blescan auto"]),
            ({"cmd": "blescan", "value": "active"}, ["blescan active"]),
            ({"cmd": "sdinfo"}, ["sdinfo"]),
            ({"cmd": "sdls"}, ["sdls"]),
            ({"cmd": "info"}, ["info"]),
            ({"cmd": "explain", "value": "clear"}, []),          # host-side only
            ({"cmd": "sdread", "path": "bandwatch-wifi-20261007-101010.pcap"},
             ["sdread /bandwatch-wifi-20261007-101010.pcap"]),
            ({"cmd": "sdread", "path": "/bandwatch-ble-20261007-101010.pcap"},
             ["sdread /bandwatch-ble-20261007-101010.pcap"]),
            ({"cmd": "sdread", "path": "bandwatch-802154-7.pcap"}, ["sdread /bandwatch-802154-7.pcap"]),
        ] + [({"cmd": "sdread", "path": n}, ["sdread /" + n]) for n in bh.CARD_TEXT_FILES]
        for body, want in cases:
            with self.subTest(body=body):
                code, sent = self.sent_for(body)
                self.assertEqual(code, 200)
                self.assertEqual(sent, want)

    def test_capture_start_stop(self):
        code, sent = self.sent_for({"cmd": "capture", "value": 1, "snaplen": 128})
        self.assertEqual((code, sent), (200, ["snap 128", "cap 1"]))
        cap = self.bw.snapshot()["capture"]
        self.assertTrue(os.path.isfile(cap["file"]))
        code, sent = self.sent_for({"cmd": "capture", "value": 0})
        self.assertEqual((code, sent), (200, ["cap 0"]))
        self.assertIsNone(self.bw.snapshot()["capture"])

    def test_rejected_commands_send_nothing(self):
        too_long = "bandwatch-wifi-" + "x" * 30 + ".pcap"   # 40 chars: the device line buffer would cut it
        cases = [
            {"cmd": "band", "value": "6g"}, {"cmd": "band"}, {"cmd": "specstep", "value": 3},
            {"cmd": "specstep"}, {"cmd": "park", "value": "six"},
            {"cmd": "hunt", "mac": "nonsense"}, {"cmd": "hunt", "mac": "aa:bb:cc:dd:ee:ff reboot"},
            {"cmd": "deauth", "mac": "aa:bb"}, {"cmd": "deauth", "mac": "aa:bb:cc:dd:ee:ff\nreboot"},
            {"cmd": "blekick", "mac": "aa:bb"}, {"cmd": "blekick", "mac": "aa:bb:cc:dd:ee:ff\nreboot"},
            {"cmd": "dca", "client_mac": "aa:aa:aa:aa:aa:aa"},
            {"cmd": "dca", "client_mac": "aa:aa:aa:aa:aa:aa 11:22:33:44:55:66", "ap_bssid": "x"},
            {"cmd": "page", "value": "up"}, {"cmd": "blescan", "value": "loud"},
            {"cmd": "ledtest", "value": "bogus"}, {"cmd": "ledtest"}, {"cmd": "ledtest", "value": "surv\nreboot"},
            {"cmd": "explain", "value": "bogus"}, {"cmd": "reboot"}, {"cmd": "nope"}, {},
            {"cmd": "sdread", "path": ""}, {"cmd": "sdread"}, {"cmd": "sdread", "path": too_long},
            {"cmd": "sdread", "path": "../etc/passwd"}, {"cmd": "sdread", "path": "foo.pcap"},
            {"cmd": "sdread", "path": "bandwatch-wifi-x y.pcap"},
            {"cmd": "sdread", "path": "bandwatch-wifi-x\nreboot.pcap"},
            {"cmd": "sdread", "path": "bandwatch-wifi-x.pcap;reboot"},
            {"cmd": "sdread", "path": "events.csv/../x"}, {"cmd": "sdread", "path": "../events.csv"},
            {"cmd": "sdread", "path": "EVENTS.CSV"}, {"cmd": "sdread", "path": "settings.bin"},
            # path traversal smuggled inside the pcap name pattern
            {"cmd": "sdread", "path": "bandwatch-wifi-../../x.pcap"},
            {"cmd": "sdread", "path": "bandwatch-wifi-a/b.pcap"},
            {"cmd": "sdread", "path": "bandwatch-wifi-a\\b.pcap"},
        ]
        for body in cases:
            with self.subTest(body=body):
                code, sent = self.sent_for(body)
                self.assertEqual(code, 400)
                self.assertEqual(sent, [])

    def test_bad_json_and_wrong_path(self):
        self.bw.ser.clear()
        self.assertEqual(self.s.post(None, raw=b"{not json")[0], 400)
        self.assertEqual(self.s.post({"cmd": "info"}, path="/api/other")[0], 404)
        self.assertEqual(self.bw.ser.lines(), [])

    def test_explain_already_running(self):
        with mock.patch.object(self.bw, "start_explain", return_value=False) as se:
            self.assertEqual(self.s.post({"cmd": "explain", "value": "full"})[0], 409)
            se.assert_called_once_with("full")


if __name__ == "__main__":
    unittest.main()
