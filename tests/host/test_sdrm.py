"""Offline tests: deleting a card file ("sdrm", docs/DEVELOPER.md section 12) - the /api/cmd mapping and its name
guard (shared with sdread), the device's ack / err handling, and the cached listing refresh."""
import os
import shutil
import unittest

from _support import bh, make_bw, feed, HELLO_24
from test_http import Server

PCAP = "bandwatch-wifi-20261007-101010.pcap"
LISTING = {"t": "sdls", "files": [[PCAP, 4096], ["bandwatch-ble-0003.pcap", 512], ["events.csv", 300],
                                   ["seen.csv", 180], ["seen.old.csv", 90000]], "total": 5, "sent": 5}


class CardFileNameTest(unittest.TestCase):
    def test_accepts_our_files(self):
        for n in [PCAP, "/" + PCAP, "bandwatch-802154-7.pcap", "bandwatch-ble-0001.pcap"] + list(bh.CARD_TEXT_FILES):
            with self.subTest(n=n):
                self.assertEqual(bh.card_file_name(n), n.lstrip("/"))
        self.assertIn("seen.old.csv", bh.CARD_TEXT_FILES)
        self.assertIn("seen.bak.csv", bh.CARD_TEXT_FILES)   # the register "seengen" parks (DEVELOPER.md 20)
        edge = "bandwatch-wifi-" + "x" * 19 + ".pcap"   # exactly 39 chars
        self.assertEqual(len(edge), 39)
        self.assertEqual(bh.card_file_name(edge), edge)

    def test_rejects_everything_else(self):
        for n in ["", "/", "//" + PCAP, "../seen.csv", "/../seen.csv", "dir/seen.csv", "/dir/" + PCAP,
                  "bandwatch-wifi-../x.pcap", "bandwatch-wifi-a/b.pcap", "bandwatch-wifi-a\\b.pcap",
                  "bandwatch-wifi-" + "x" * 20 + ".pcap", "SEEN.CSV", "settings.bin", "foo.pcap",
                  "seen.csv\nreboot", "events.csv ", None, 7, ["seen.csv"]]:
            with self.subTest(n=n):
                self.assertIsNone(bh.card_file_name(n))


class SdrmHttpTest(unittest.TestCase):
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

    def test_mapping(self):
        names = [PCAP, "bandwatch-ble-0003.pcap", "bandwatch-802154-7.pcap"] + list(bh.CARD_TEXT_FILES)
        for n in names:
            for given in (n, "/" + n):
                with self.subTest(path=given):
                    code, sent = self.sent_for({"cmd": "sdrm", "path": given})
                    self.assertEqual((code, sent), (200, ["sdrm /" + n]))
                    rm = self.bw.snapshot()["sd_rm"]
                    self.assertEqual((rm["name"], rm["pending"]), (n, True))

    def test_rejected_send_nothing(self):
        too_long = "bandwatch-wifi-" + "x" * 20 + ".pcap"   # 40 chars: "sdrm /" + name would be cut on the device
        for body in [{"cmd": "sdrm"}, {"cmd": "sdrm", "path": ""}, {"cmd": "sdrm", "path": "/"},
                     {"cmd": "sdrm", "path": too_long}, {"cmd": "sdrm", "path": "../seen.csv"},
                     {"cmd": "sdrm", "path": "/captures/" + PCAP}, {"cmd": "sdrm", "path": "events.csv/../x"},
                     {"cmd": "sdrm", "path": "bandwatch-wifi-../../x.pcap"}, {"cmd": "sdrm", "path": "settings.bin"},
                     {"cmd": "sdrm", "path": "seen.csv\nsdrm /events.csv"}, {"cmd": "sdrm", "path": 5}]:
            with self.subTest(body=body):
                code, sent = self.sent_for(body)
                self.assertEqual((code, sent), (400, []))
        self.assertIsNone(self.bw.snapshot()["sd_rm"])


class SdrmProtocolTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)
        feed(self.bw, HELLO_24)
        feed(self.bw, LISTING)
        self.bw.ser.clear()

    def names(self):
        return [n for n, _ in self.bw.snapshot()["sd"]["files"]]

    def test_ack_ok_drops_file_and_relists(self):
        bw = self.bw
        bw.sd_rm_request(PCAP)
        feed(bw, {"t": "ack", "cmd": "sdrm", "file": "/" + PCAP, "ok": 1})
        self.assertNotIn(PCAP, self.names())
        self.assertEqual(len(self.names()), 4)
        self.assertEqual(bw.snapshot()["sd"]["file_total"], 4)
        self.assertEqual(bw.ser.lines(), ["sdls"])
        rm = bw.snapshot()["sd_rm"]
        self.assertEqual((rm["name"], rm["pending"], rm["ok"]), (PCAP, False, True))
        # the device's fresh listing replaces the cache
        feed(bw, {"t": "sdls", "files": [["events.csv", 300]], "total": 1, "sent": 1})
        self.assertEqual(self.names(), ["events.csv"])

    def test_ack_failure_keeps_listing(self):
        bw = self.bw
        bw.sd_rm_request("seen.csv")
        feed(bw, {"t": "ack", "cmd": "sdrm", "file": "/seen.csv", "ok": 0, "msg": "no such file"})
        self.assertIn("seen.csv", self.names())
        self.assertEqual(bw.ser.lines(), [])
        rm = bw.snapshot()["sd_rm"]
        self.assertEqual((rm["name"], rm["pending"], rm["ok"], rm["msg"]), ("seen.csv", False, False, "no such file"))

    def test_refusal_err_line(self):
        bw = self.bw
        bw.sd_rm_request(PCAP)
        feed(bw, {"t": "err", "msg": "sdrm: /%s is being recorded - stop sdcap first" % PCAP})
        rm = bw.snapshot()["sd_rm"]
        self.assertEqual((rm["name"], rm["pending"], rm["ok"]), (PCAP, False, False))
        self.assertIn("being recorded", rm["msg"])
        self.assertIn(PCAP, self.names())
        self.assertEqual(bw.ser.lines(), [])
        for msg in ("sdrm: no card", "sdrm: busy - a file is being pulled (sdread)",
                    "sdrm: busy - the event log is writing to the card", "sdrm: bad file name (card root only, <= 39 chars)"):
            bw.sd_rm_request("events.csv")
            feed(bw, {"t": "err", "msg": msg})
            self.assertFalse(bw.snapshot()["sd_rm"]["ok"])
            self.assertFalse(bw.snapshot()["sd_rm"]["pending"])

    def test_local_pulled_copy_stays(self):
        bw = self.bw
        local = os.path.join(bw.captures_dir, "events.csv")
        with open(local, "w") as f:
            f.write("pulled earlier\n")
        feed(bw, {"t": "ack", "cmd": "sdrm", "file": "/events.csv", "ok": 1})
        self.assertTrue(os.path.isfile(local))
        self.assertNotIn("events.csv", self.names())

    def test_ack_without_listing_or_request(self):
        bw = make_bw()
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        feed(bw, {"t": "ack", "cmd": "sdrm", "file": "/seen.csv", "ok": 1})   # no hello, no sdls yet
        self.assertEqual(bw.snapshot()["sd_rm"]["name"], "seen.csv")
        self.assertEqual(bw.ser.lines(), ["sdls"])
        feed(bw, {"t": "ack", "cmd": "sdrm", "ok": 1, "file": None})        # malformed: must not raise
        feed(bw, {"t": "err", "msg": "sdrm:"})
        bw.snapshot()

    def test_failed_sdread_is_not_in_progress(self):
        bw = self.bw
        feed(bw, {"t": "ack", "cmd": "sdread", "file": "/" + PCAP, "bytes": 10})
        self.assertFalse(bw.snapshot()["sd_read"]["failed"])
        feed(bw, {"t": "err", "msg": "sdread: read failed at 0 of 10 bytes (card removed?)"})
        st = bw.snapshot()["sd_read"]
        self.assertEqual((st["done"], st["failed"]), (False, True))


if __name__ == "__main__":
    unittest.main()
