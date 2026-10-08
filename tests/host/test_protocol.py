"""Offline tests: the host's handling of the device's JSON lines (hello, d, s, w, b, z, pr, ev, log/err, fs/fd)."""
import copy
import os
import unittest
from unittest import mock

from _support import bh, make_bw, feed, FakeClock, HELLO_24, wifi_row


class HelloTest(unittest.TestCase):
    def test_hello_populates_state(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        snap = bw.snapshot()
        self.assertEqual(snap["band"], "2.4g")
        self.assertEqual([c["ch"] for c in snap["channels"]], list(range(1, 14)))
        self.assertEqual(snap["heap"], 88123)
        self.assertEqual(snap["park"], 0)
        self.assertEqual(snap["cap"], 0)
        self.assertEqual(snap["hello"]["ver"], "1.18")
        self.assertEqual(snap["sd"]["mounted"], 1)
        self.assertEqual(snap["events"]["on"], 1)
        self.assertEqual(snap["spec_step"], 2)
        self.assertIsNone(snap["hunt"])
        self.assertIsNone(snap["deauth"])
        # placeholder channel rows say "no data yet"
        self.assertTrue(all(c["state"] == 1 for c in snap["channels"]))

    def test_hello_band_change_drops_channels_not_in_new_list(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        h = dict(HELLO_24, band="154", chs=list(range(11, 27)))
        feed(bw, h)
        self.assertEqual([c["ch"] for c in bw.snapshot()["channels"]], list(range(11, 27)))

    def test_hello_hunt_and_deauth(self):
        bw = make_bw()
        h = dict(HELLO_24, hunt="aa:bb:cc:dd:ee:ff", h=[-48, 120, 7], deauth=["11:22:33:44:55:66", 6, 40, 1])
        feed(bw, h)
        snap = bw.snapshot()
        self.assertEqual(snap["hunt"]["mac"], "aa:bb:cc:dd:ee:ff")
        self.assertEqual(snap["hunt"]["rssi"], -48)
        self.assertEqual(snap["hunt"]["count"], 7)
        self.assertEqual(snap["deauth"], {"mac": "11:22:33:44:55:66", "ch": 6, "sent": 40, "fail": 1,
                                          "targeted": False})

    def test_hello_targeted_deauth(self):
        bw = make_bw()
        feed(bw, dict(HELLO_24, deauth=["aa:aa:aa:aa:aa:aa", "11:22:33:44:55:66", 1, 9, 0]))
        d = bw.snapshot()["deauth"]
        self.assertTrue(d["targeted"])
        self.assertEqual(d["ap_bssid"], "11:22:33:44:55:66")
        self.assertEqual(d["sent"], 9)

    def test_hello_band_change_stops_host_capture(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        bw.start_capture()
        self.assertIsNotNone(bw.snapshot()["capture"])
        feed(bw, dict(HELLO_24, band="ble", chs=[]))   # BOOT-button band change on the device
        snap = bw.snapshot()
        self.assertIsNone(snap["capture"])
        self.assertEqual(snap["saved"]["usb"]["count"], 1)
        self.assertIn("cap 0", bw.ser.lines())


class DwellSweepTest(unittest.TestCase):
    D6 = {"t": "d", "c": 6, "s": 42.5, "r": 50.0, "f": 120, "b": 30000, "st": 3, "u": 7, "g": 42.5, "n": 3,
          "park": 0, "cap": 0, "drop": 0, "da": 0, "df": 0, "sdc": 0, "sdf": 0, "sdb": 0,
          "top": "aa:bb:cc:00:11:22", "trssi": -40, "h": None}

    def test_dwell_line(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, self.D6)
        snap = bw.snapshot()
        ch6 = next(c for c in snap["channels"] if c["ch"] == 6)
        self.assertEqual(ch6["s"], 42.5)
        self.assertEqual(ch6["f"], 120)
        self.assertEqual(ch6["state"], 0)
        self.assertEqual(ch6["top"], "aa:bb:cc:00:11:22")
        self.assertEqual(ch6["trssi"], -40)
        self.assertEqual(snap["current"], 6)
        self.assertEqual(snap["global"], 42.5)
        self.assertEqual(snap["sweep"], 3)
        self.assertEqual(len(snap["history"]), 1)
        t, g, c, s, avg = snap["history"][0]
        self.assertEqual((g, c, s, avg), (42.5, 6, 42.5, 42.5))

    def test_dwell_without_top_talker(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, dict(self.D6, top=None, trssi=None))
        ch6 = next(c for c in bw.snapshot()["channels"] if c["ch"] == 6)
        self.assertNotIn("top", ch6)

    def test_dwell_sd_counters_update_sd_block(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, dict(self.D6, sdc=1, sdf=50, sdb=9000))
        sd = bw.snapshot()["sd"]
        self.assertEqual((sd["cap"], sd["frames"], sd["bytes"]), (1, 50, 9000))

    def test_sweep_line(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, self.D6)
        feed(bw, {"t": "s", "n": 4, "g": 50.0, "band": "2.4g",
                  "ch": [[1, 10.0, 5, 500, 0, 1, 0], [6, 40.0, 130, 31000, 3, 8, 0], [13, 0.0, 0, 0, 0, 0, 2]],
                  "aps": 3, "drop": 1, "heap": 87000})
        snap = bw.snapshot()
        self.assertEqual([c["ch"] for c in snap["channels"]], [1, 6, 13])
        ch6 = snap["channels"][1]
        self.assertEqual(ch6["s"], 40.0)
        self.assertEqual(ch6["r"], 50.0)                     # raw score kept from the dwell
        self.assertEqual(ch6["top"], "aa:bb:cc:00:11:22")     # C6 top talker survives the sweep row
        self.assertEqual(snap["channels"][2]["state"], 2)
        self.assertEqual((snap["sweep"], snap["aps"], snap["drop"], snap["heap"]), (4, 3, 1, 87000))

    def test_fine_spectrum_fs_and_fd(self):
        bw = make_bw()
        feed(bw, dict(HELLO_24, band="spec", chs=list(range(11, 27))))
        bins = [[-110, -108, -105, 10] for _ in range(42)]
        feed(bw, {"t": "fs", "n": 1, "step": 2, "lo": 2400, "count": 42, "bins": bins, "heap": 90000})
        snap = bw.snapshot()
        self.assertEqual(len(snap["fine"]["bins"]), 42)
        self.assertEqual(snap["fine"]["bins"][13]["mhz"], 2426)
        feed(bw, {"t": "fd", "mhz": 2426, "min": -90, "mean": -70, "max": -50, "ns": 12, "step": 2, "n": 1})
        snap = bw.snapshot()
        self.assertEqual(snap["fine"]["current_mhz"], 2426)
        self.assertEqual(snap["fine"]["bins"][13]["max"], -50)
        self.assertEqual(snap["fine"]["bins"][12]["max"], -105)


class WifiTableTest(unittest.TestCase):
    def test_chunked_w_lines_merge_by_mac(self):
        """v1.17: a full table arrives as several <=24-row lines; the host merges them by MAC."""
        bw = make_bw()
        macs = ["02:00:00:00:00:%02x" % i for i in range(40)]
        feed(bw, {"t": "w", "dev": [wifi_row(m, rssi=-40 - i) for i, m in enumerate(macs[:24])]})
        feed(bw, {"t": "w", "dev": [wifi_row(m, rssi=-70 - i) for i, m in enumerate(macs[24:])]})
        devs = {d["mac"]: d for d in bw.snapshot()["wifi_devs"]}
        self.assertEqual(set(devs), set(macs))
        # a later chunk updates a row from an earlier one instead of duplicating it
        feed(bw, {"t": "w", "dev": [wifi_row(macs[0], rssi=-33, frames=99)]})
        devs = {d["mac"]: d for d in bw.snapshot()["wifi_devs"]}
        self.assertEqual(len(devs), 40)
        self.assertEqual(devs[macs[0]]["rssi"], -33)
        self.assertEqual(devs[macs[0]]["frames"], 99)

    def test_ap_row_ies(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row("11:22:33:44:55:66", flags=3, ssid="HomeNet", sec=4, pmf=1, phy=2,
                                             bw=4, util=51, stations=5, cc="SE")]})
        d = bw.snapshot()["wifi_devs"][0]
        self.assertTrue(d["ap"])
        self.assertEqual(d["ssid"], "HomeNet")
        self.assertEqual(d["sec"], "WPA2-PSK · PMF capable")
        self.assertEqual(d["phy"], "n (Wi-Fi 4)")
        self.assertEqual((d["bw"], d["util"], d["stations"], d["cc"]), (40, 20, 5, "SE"))

    def test_ssid_is_sticky_when_a_later_row_has_none(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row("11:22:33:44:55:66", flags=1, ssid="HomeNet")]})
        feed(bw, {"t": "w", "dev": [wifi_row("11:22:33:44:55:66", flags=1, ssid="")]})
        self.assertEqual(bw.snapshot()["wifi_devs"][0]["ssid"], "HomeNet")

    def test_tier1_dest_only(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [
            wifi_row("a4:b1:c1:00:00:01", rssi=0, mx=0, flags=4),                 # addr1-only, no surv
            wifi_row("a4:b1:c1:00:00:02", rssi=0, mx=0, flags=4, surv=1),         # addr1-only surveillance OUI
            wifi_row("a4:b1:c1:00:00:03", rssi=-60, flags=0, surv=4),             # heard transmitting
        ]})
        devs = {d["mac"]: d for d in bw.snapshot()["wifi_devs"]}
        self.assertTrue(devs["a4:b1:c1:00:00:01"]["dest_only"])
        self.assertEqual(devs["a4:b1:c1:00:00:01"]["tier"], 0)
        self.assertEqual(devs["a4:b1:c1:00:00:02"]["tier"], 1)
        self.assertEqual(devs["a4:b1:c1:00:00:02"]["surv"], "Flock Safety")
        self.assertFalse(devs["a4:b1:c1:00:00:03"]["dest_only"])
        self.assertEqual(devs["a4:b1:c1:00:00:03"]["tier"], 2)
        self.assertEqual(devs["a4:b1:c1:00:00:03"]["surv_kind"], "drone")

    def test_old_8_field_rows_still_merge(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [["11:22:33:44:55:66", -50, -45, 10, 500, 6, 1, "Old"]]})
        d = bw.snapshot()["wifi_devs"][0]
        self.assertEqual(d["ssid"], "Old")
        self.assertIsNone(d["parent"])


class ResolveParentsTest(unittest.TestCase):
    AP = "11:22:33:44:55:66"

    def _devs(self, bw):
        return {d["mac"]: d for d in bw.snapshot()["wifi_devs"]}

    def test_station_joins_known_ap(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row(self.AP, flags=1, ssid="HomeNet"),
                                    wifi_row("a4:b1:c1:00:00:01", ap_suffix="445566")]})
        self.assertEqual(self._devs(bw)["a4:b1:c1:00:00:01"]["parent"], self.AP)

    def test_join_completes_when_the_ap_arrives_in_a_later_chunk(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row("a4:b1:c1:00:00:01", ap_suffix="445566")]})
        self.assertIsNone(self._devs(bw)["a4:b1:c1:00:00:01"]["parent"])
        feed(bw, {"t": "w", "dev": [wifi_row(self.AP, flags=1)]})
        self.assertEqual(self._devs(bw)["a4:b1:c1:00:00:01"]["parent"], self.AP)

    def test_ambiguous_suffix_refuses_to_guess(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row(self.AP, flags=1), wifi_row("aa:bb:cc:44:55:66", flags=1),
                                    wifi_row("a4:b1:c1:00:00:01", ap_suffix="445566")]})
        self.assertIsNone(self._devs(bw)["a4:b1:c1:00:00:01"]["parent"])

    def test_unknown_suffix_and_self_reference(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row(self.AP, flags=1, ap_suffix="445566"),
                                    wifi_row("a4:b1:c1:00:00:01", ap_suffix="999999")]})
        devs = self._devs(bw)
        self.assertIsNone(devs[self.AP]["parent"])                 # an AP never parents itself
        self.assertIsNone(devs["a4:b1:c1:00:00:01"]["parent"])     # suffix of an AP we never heard

    def test_suffix_is_sticky(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row(self.AP, flags=1), wifi_row("a4:b1:c1:00:00:01", ap_suffix="445566")]})
        feed(bw, {"t": "w", "dev": [wifi_row("a4:b1:c1:00:00:01", ap_suffix="")]})
        self.assertEqual(self._devs(bw)["a4:b1:c1:00:00:01"]["parent"], self.AP)


class BleAnd154Test(unittest.TestCase):
    def test_ble_rows(self):
        bw = make_bw()
        feed(bw, {"t": "b", "dev": [
            ["c0:11:22:33:44:55", -70, -65, 30, 1000, 1, 0x004C, "", 0, 127, 0, 0, 0x12, 1, 0],
            ["00:1b:66:00:00:01", -55, -50, 12, 200, 0, 0x009E, "Bose QC", (37 << 6) | 1, -8, 0, 0, 0, 2, 0],
            ["90:3a:e6:00:00:02", -60, -58, 5, 300, 0, 0, "", 0, 127, 0xFEAA, 0, 0, 0, 2],
        ]})
        devs = {d["mac"]: d for d in bw.snapshot()["ble_devs"]}
        tag = devs["c0:11:22:33:44:55"]
        self.assertEqual(tag["company_name"], "Apple")
        self.assertEqual(tag["kind"], "Find My (AirTag / Find My network)")
        self.assertTrue(tag["random"])
        self.assertIsNone(tag["tx"])
        self.assertTrue(tag["connectable"])
        bose = devs["00:1b:66:00:00:01"]
        self.assertEqual(bose["name"], "Bose QC")
        self.assertEqual(bose["kind"], "Wearable audio")
        self.assertEqual(bose["tx"], -8)
        self.assertTrue(bose["legacy"])
        ring = devs["90:3a:e6:00:00:02"]
        self.assertEqual(ring["kind"], "Eddystone beacon")
        self.assertEqual((ring["surv"], ring["tier"]), ("Ring", 2))

    def test_ble_heartbeat(self):
        bw = make_bw()
        feed(bw, {"t": "ble", "devs": 14, "cycles": 1, "heap": 91000, "adv": 400, "scan": "auto",
                  "running": "passive", "switches": 2, "cap": 1, "drop": 3, "sdc": 1, "sdf": 7, "sdb": 700, "h": None})
        snap = bw.snapshot()
        self.assertEqual(snap["ble"]["devs"], 14)
        self.assertEqual(snap["ble"]["running"], "passive")
        self.assertEqual((snap["cap"], snap["drop"], snap["heap"]), (1, 3, 91000))
        self.assertEqual(snap["sd"]["frames"], 7)

    def test_154_rows(self):
        bw = make_bw()
        feed(bw, {"t": "z", "dev": [
            ["00:12:4b:00:01:02:03:04", -60, -55, 40, 500, 15, 0x1A62, 0x0000, 1, 1 | 16, 200],
            ["1a62/1234", -75, -70, 3, 900, 15, 0x1A62, 0x1234, 3, 2 | 4 | 8, 90],
            ["ffff/ffff", -80, -80, 1, 900, 20, 0xFFFF, 0xFFFF, 0, 0, 10],
        ]})
        devs = {d["key"]: d for d in bw.snapshot()["z_devs"]}
        ext = devs["00:12:4b:00:01:02:03:04"]
        self.assertEqual(ext["ext"], "00:12:4b:00:01:02:03:04")
        self.assertEqual((ext["pan"], ext["short"], ext["proto"]), ("1a62", "0000", "Zigbee"))
        self.assertTrue(ext["data"])
        short = devs["1a62/1234"]
        self.assertEqual(short["ext"], "")
        self.assertEqual(short["proto"], "Thread / 6LoWPAN")
        self.assertTrue(short["beacons"] and short["permit_join"] and short["mac_secured"])
        bcast = devs["ffff/ffff"]
        self.assertIsNone(bcast["pan"])
        self.assertIsNone(bcast["short"])


class ProbeTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.patch = mock.patch.object(bh.time, "time", self.clock)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def pr(self, bw, mac, ssid, rssi=-60, ch=6):
        feed(bw, {"t": "pr", "mac": mac, "rssi": rssi, "ch": ch, "ssid": ssid})

    def test_grouping_by_ssid_and_random_macs(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row("11:22:33:44:55:66", flags=1, ssid="HomeNet")]})
        self.pr(bw, "da:00:00:00:00:01", "HomeNet", rssi=-70)   # locally administered = randomized
        self.pr(bw, "6e:00:00:00:00:02", "HomeNet", rssi=-50)   # randomized too (0x6e & 2)
        self.pr(bw, "a4:b1:c1:00:00:03", "HomeNet", rssi=-65)   # a real (global) MAC
        self.pr(bw, "a4:b1:c1:00:00:03", "CoffeeShop")
        groups = {g["ssid"]: g for g in bw.snapshot()["probes"]}
        home = groups["HomeNet"]
        self.assertEqual((home["macs"], home["random"]), (3, 2))
        self.assertEqual(home["rssi"], -50)
        self.assertTrue(home["nearby_ap"])
        self.assertEqual([s["mac"] for s in home["sample"]], ["a4:b1:c1:00:00:03"])   # no random MACs sampled
        self.assertFalse(groups["CoffeeShop"]["nearby_ap"])
        self.assertEqual(groups["CoffeeShop"]["macs"], 1)

    def test_probe_without_ssid_or_mac_is_ignored(self):
        bw = make_bw()
        feed(bw, {"t": "pr", "mac": "a4:b1:c1:00:00:03", "rssi": -60, "ch": 6, "ssid": ""})
        feed(bw, {"t": "pr", "rssi": -60, "ch": 6, "ssid": "X"})
        self.assertEqual(bw.snapshot()["probes"], [])

    def test_expiry_after_15_minutes(self):
        bw = make_bw()
        self.pr(bw, "a4:b1:c1:00:00:03", "Old")
        self.clock.advance(600)
        self.pr(bw, "a4:b1:c1:00:00:03", "Fresh")
        self.clock.advance(bh.PROBE_TTL_S - 600 + 1)        # "Old" is now 901 s old, "Fresh" 301 s
        groups = {g["ssid"]: g for g in bw.snapshot()["probes"]}
        self.assertNotIn("Old", groups)
        self.assertIn("Fresh", groups)
        self.assertAlmostEqual(groups["Fresh"]["age"], 301, delta=0.2)
        self.clock.advance(bh.PROBE_TTL_S)
        self.assertEqual(bw.snapshot()["probes"], [])
        self.assertEqual(bw.probes, {})                    # the MAC itself is forgotten too

    def test_reannounce_refreshes_expiry(self):
        bw = make_bw()
        self.pr(bw, "a4:b1:c1:00:00:03", "HomeNet")
        self.clock.advance(800)
        self.pr(bw, "a4:b1:c1:00:00:03", "HomeNet")
        self.clock.advance(800)
        self.assertEqual([g["ssid"] for g in bw.snapshot()["probes"]], ["HomeNet"])

    def test_seeking_on_device_rows(self):
        bw = make_bw()
        feed(bw, {"t": "w", "dev": [wifi_row("a4:b1:c1:00:00:03"), wifi_row("a4:b1:c1:00:00:04")]})
        self.pr(bw, "a4:b1:c1:00:00:03", "Zeta")
        self.pr(bw, "a4:b1:c1:00:00:03", "Alpha")
        devs = {d["mac"]: d for d in bw.snapshot()["wifi_devs"]}
        self.assertEqual(devs["a4:b1:c1:00:00:03"]["seeking"], ["Alpha", "Zeta"])
        self.assertEqual(devs["a4:b1:c1:00:00:04"]["seeking"], [])

    def test_probe_table_is_capped(self):
        bw = make_bw()
        with mock.patch.object(bh, "PROBE_MAX", 3):
            for i in range(5):
                self.clock.advance(1)
                self.pr(bw, "a4:b1:c1:00:00:%02x" % i, "Net%d" % i)
        self.assertEqual(sorted(bw.probes), ["a4:b1:c1:00:00:02", "a4:b1:c1:00:00:03", "a4:b1:c1:00:00:04"])


class EventsTest(unittest.TestCase):
    EV_OFF = {"on": 0, "card": 0, "base": 0, "file": 0, "written": 0, "pending": 0, "surv": 0, "new": 0,
              "drop": 0, "err": 0, "wait": 0}

    def test_ev_in_hello_ack_and_status_line(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        self.assertEqual(bw.snapshot()["events"]["written"], 3)
        feed(bw, {"t": "ack", "cmd": "events", "ev": self.EV_OFF})
        self.assertEqual(bw.snapshot()["events"], self.EV_OFF)
        feed(bw, {"t": "ev", "ev": dict(self.EV_OFF, on=1, pending=4, wait=2)})
        ev = bw.snapshot()["events"]
        self.assertEqual((ev["on"], ev["pending"], ev["wait"]), (1, 4, 2))

    def test_old_firmware_without_ev_keeps_none(self):
        bw = make_bw()
        h = copy.deepcopy(HELLO_24)
        del h["ev"]
        feed(bw, h)
        self.assertIsNone(bw.snapshot()["events"])
        feed(bw, {"t": "ev", "ev": "garbage"})
        self.assertIsNone(bw.snapshot()["events"])


class SdStateTest(unittest.TestCase):
    def test_sdcap_recording_stopped_clears_cap_and_counts_file(self):
        bw = make_bw()
        h = copy.deepcopy(HELLO_24)
        h["sd"].update(cap=1, file="/bandwatch-wifi-20261007-101010.pcap", frames=600, bytes=160000)
        feed(bw, h)
        self.assertEqual(bw.snapshot()["sd"]["cap"], 1)
        feed(bw, {"t": "err", "msg": "sdcap: write failed after 619 frames (card removed?) - recording stopped"})
        snap = bw.snapshot()
        self.assertEqual(snap["sd"]["cap"], 0)
        self.assertEqual(snap["saved"]["sd"]["count"], 1)
        self.assertEqual(snap["saved"]["sd"]["last"], "/bandwatch-wifi-20261007-101010.pcap")

    def test_other_sdcap_errors_leave_cap_alone(self):
        bw = make_bw()
        h = copy.deepcopy(HELLO_24)
        h["sd"].update(cap=1, file="/x.pcap")
        feed(bw, h)
        feed(bw, {"t": "err", "msg": "sdcap: no capture ring"})
        self.assertEqual(bw.snapshot()["sd"]["cap"], 1)

    def test_card_removed_and_inserted_logs(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, {"t": "sdls", "files": [["bandwatch-wifi-1.pcap", 100]], "total": 1, "sent": 1})
        self.assertEqual(bw.snapshot()["sd"]["files"], [["bandwatch-wifi-1.pcap", 100]])
        feed(bw, {"t": "log", "msg": "sd card removed: recording stopped"})
        sd = bw.snapshot()["sd"]
        self.assertEqual(sd["mounted"], 0)
        self.assertNotIn("files", sd)            # the listing belonged to the card that left
        feed(bw, {"t": "log", "msg": "sd card inserted"})
        self.assertEqual(bw.snapshot()["sd"]["mounted"], 1)

    def test_card_log_before_any_hello(self):
        bw = make_bw()
        feed(bw, {"t": "log", "msg": "sd card inserted"})
        self.assertEqual(bw.snapshot()["sd"], {"mounted": 1})

    def test_sdcap_and_sdinfo_acks(self):
        bw = make_bw()
        feed(bw, {"t": "ack", "cmd": "sdinfo", "sd": 1, "mb": 30436, "used_mb": 12, "cap": 0, "file": "",
                  "frames": 0, "bytes": 0, "err": 0})
        self.assertEqual(bw.snapshot()["sd"]["mounted"], 1)
        feed(bw, {"t": "ack", "cmd": "sdcap", "sdcap": 1, "file": "/bandwatch-wifi-2.pcap"})
        sd = bw.snapshot()["sd"]
        self.assertEqual((sd["cap"], sd["file"]), (1, "/bandwatch-wifi-2.pcap"))
        feed(bw, {"t": "ack", "cmd": "sdcap", "sdcap": 0, "file": ""})
        self.assertEqual(bw.snapshot()["saved"]["sd"]["count"], 1)


class AckTest(unittest.TestCase):
    def test_band_park_cap_hunt_dca_acks(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, {"t": "ack", "cmd": "park", "park": 6})
        feed(bw, {"t": "ack", "cmd": "cap", "cap": 1})
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": "aa:bb:cc:dd:ee:ff", "park": 6})
        feed(bw, {"t": "ack", "cmd": "dca", "deauth": ["aa:aa:aa:aa:aa:aa", "11:22:33:44:55:66", 1, 0, 0],
                  "park": 6, "fc": "0xc0"})
        snap = bw.snapshot()
        self.assertEqual((snap["park"], snap["cap"]), (6, 1))
        self.assertEqual(snap["hunt"]["mac"], "aa:bb:cc:dd:ee:ff")
        self.assertTrue(snap["deauth"]["targeted"])
        feed(bw, {"t": "ack", "cmd": "dca", "deauth": None, "park": 0})
        feed(bw, {"t": "ack", "cmd": "hunt", "hunt": None, "park": 0})
        feed(bw, {"t": "ack", "cmd": "band", "band": "ble"})
        snap = bw.snapshot()
        self.assertIsNone(snap["deauth"])
        self.assertIsNone(snap["hunt"])
        self.assertEqual(snap["band"], "ble")


class BleKickTest(unittest.TestCase):
    """BLE kick (v1.21): the "bk" member rides the BLE heartbeat and the blekick ack; absent on older firmware."""

    def test_ble_heartbeat_carries_bkick(self):
        bw = make_bw()
        feed(bw, {"t": "ble", "devs": 3, "cycles": 9, "heap": 80000, "adv": 120, "scan": "active",
                  "running": "active", "switches": 1, "cap": 0, "drop": 0, "sdc": 0, "sdf": 0, "sdb": 0,
                  "h": None, "bk": ["aa:bb:cc:dd:ee:ff", "connected", 3, 1]})
        self.assertEqual(bw.snapshot()["blekick"], {"mac": "aa:bb:cc:dd:ee:ff", "state": "connected",
                                                     "kicks": 3, "fails": 1})

    def test_ble_heartbeat_null_bkick_clears(self):
        bw = make_bw()
        line = {"t": "ble", "devs": 3, "cycles": 9, "heap": 80000, "adv": 120, "scan": "active",
                "running": "active", "switches": 1, "cap": 0, "drop": 0, "sdc": 0, "sdf": 0, "sdb": 0, "h": None}
        feed(bw, dict(line, bk=["aa:bb:cc:dd:ee:ff", "idle", 0, 2]))
        self.assertIsNotNone(bw.snapshot()["blekick"])
        feed(bw, dict(line, bk=None))   # a stop on the device shows up within one heartbeat (<= 1 s)
        self.assertIsNone(bw.snapshot()["blekick"])

    def test_blekick_ack(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        feed(bw, {"t": "ack", "cmd": "blekick", "bk": ["aa:bb:cc:dd:ee:ff", "idle", 0, 0]})
        self.assertEqual(bw.snapshot()["blekick"]["mac"], "aa:bb:cc:dd:ee:ff")
        feed(bw, {"t": "ack", "cmd": "blekick", "bk": None})   # stopped (or started outside BLE mode)
        self.assertIsNone(bw.snapshot()["blekick"])

    def test_old_firmware_without_bk_key_keeps_none(self):
        bw = make_bw()
        feed(bw, {"t": "ble", "devs": 3, "cycles": 9, "heap": 80000, "adv": 120, "scan": "auto",
                  "running": "passive", "switches": 1, "cap": 0, "drop": 0, "sdc": 0, "sdf": 0, "sdb": 0, "h": None})
        self.assertIsNone(bw.snapshot()["blekick"])


class AlertsTest(unittest.TestCase):
    """LED alert blips (v1.19): "alerts" on hello and on the alerts ack; absent on older firmware."""

    def test_alerts_from_hello_and_ack(self):
        bw = make_bw()
        feed(bw, dict(copy.deepcopy(HELLO_24), alerts=1))
        self.assertEqual(bw.snapshot()["alerts"], 1)
        feed(bw, {"t": "ack", "cmd": "alerts", "alerts": 0})
        self.assertEqual(bw.snapshot()["alerts"], 0)
        feed(bw, {"t": "ack", "cmd": "ledtest", "kind": "surv", "shown": 1})   # a test blip changes nothing
        self.assertEqual(bw.snapshot()["alerts"], 0)

    def test_old_firmware_without_alerts_keeps_none(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        self.assertIsNone(bw.snapshot()["alerts"])


if __name__ == "__main__":
    unittest.main()
