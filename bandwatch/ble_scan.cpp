// BLE scanning for Bandwatch: drives NimBLE's ble_gap_disc() directly instead of going through the Arduino
// BLEScan wrapper. The wrapper cannot give packet-accurate data: parseAdvertisement() *merges* every payload it
// sees from an address ("handles both ADV and Scan Response packets by merging them"), onResult() fires only
// once a scan response arrives when active scanning, and setAdvType() is recorded only on first sight. Driving
// the GAP API ourselves hands us one callback per on-air advertising report, with its own data, address, RSSI
// and PDU type - which is what a pcap needs. It also removes the library's result cache, and with it the
// heap-floor dance serviceBle() used to need.
#include "bandwatch_core.h"

#include <BLEDevice.h>
#include <host/ble_gap.h>
#include <nimble/hci_common.h>
#include "surv_ouis.h"   // surveillance-OUI table (matched in firmware so the LCD can flag without a host)

BleState bleScan;   // scan policy + bookkeeping (type and comment in bandwatch_core.h)
const char* bleScanModeName() {
    return bleScan.mode == BLE_SCAN_ACTIVE ? "active" : bleScan.mode == BLE_SCAN_PASSIVE ? "passive" : "auto";
}

namespace {

// ---------------------------------------------------------------------------------------------
// RX path (runs in the NimBLE host task: parse, track, optionally copy – nothing else)
// ---------------------------------------------------------------------------------------------
// PDU types are BT Core Spec Vol 6 Part B 2.3; the access address is kAdvAccessAddr from bandwatch_core.h.
enum : uint8_t { LL_ADV_IND = 0x0, LL_ADV_DIRECT_IND = 0x1, LL_ADV_NONCONN_IND = 0x2,
                 LL_SCAN_RSP = 0x4, LL_ADV_SCAN_IND = 0x6 };

// HCI advertising-report event types are NOT the same numbers as LL PDU types - map them.
uint8_t llPduTypeFromHci(uint8_t evtype) {
    switch (evtype) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:     return LL_ADV_IND;
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     return LL_ADV_DIRECT_IND;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:    return LL_ADV_SCAN_IND;
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: return LL_ADV_NONCONN_IND;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:    return LL_SCAN_RSP;
        default:                                 return LL_ADV_IND;
    }
}

// Minimal AD-structure parser: [len][type][data...]. Replaces what BLEAdvertisedDevice used to do for
// us, and unlike the library's version it only ever sees one packet's worth of data.
struct AdInfo {
    char name[21];
    uint16_t company, appearance, svc, svcData;
    int8_t txPower;
    uint8_t appleType;
    bool haveName;
};

void parseAdStructures(const uint8_t* p, uint16_t n, AdInfo& out) {
    memset(&out, 0, sizeof(out));
    out.txPower = 127;
    uint16_t i = 0;
    while (i + 1 < n) {
        const uint8_t len = p[i];
        if (len == 0 || i + 1 + len > n) break;
        const uint8_t type = p[i + 1];
        const uint8_t* v = p + i + 2;
        const uint8_t vlen = len - 1;
        switch (type) {
            case 0x08: case 0x09: {                       // shortened / complete local name
                const uint8_t c = vlen < sizeof(out.name) - 1 ? vlen : sizeof(out.name) - 1;
                memcpy(out.name, v, c); out.name[c] = 0;
                sanitizeText(out.name, c);
                out.haveName = true;
                break;
            }
            case 0x0A: if (vlen >= 1) out.txPower = static_cast<int8_t>(v[0]); break;   // TX power level
            case 0x19: if (vlen >= 2) out.appearance = v[0] | (v[1] << 8); break;       // appearance
            case 0x02: case 0x03:                                                       // 16-bit service UUIDs
                if (vlen >= 2 && !out.svc) out.svc = v[0] | (v[1] << 8);
                break;
            case 0x16:                                                                  // 16-bit service data
                if (vlen >= 2 && !out.svcData) out.svcData = v[0] | (v[1] << 8);
                break;
            case 0xFF:                                                                  // manufacturer specific
                if (vlen >= 2) {
                    out.company = v[0] | (v[1] << 8);
                    if (out.company == 0x004C && vlen >= 3) out.appleType = v[2];
                }
                break;
            default: break;
        }
        i += 1 + len;
    }
}

// Build one LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR record body (without the 10-byte pseudo-header, which
// sdWriteFrame/the host writer prepend from f.channel and f.rssi, exactly as radiotap is prepended for
// Wi-Fi): access address, LL header, AdvA, [TargetA], AdvData, CRC.
uint16_t buildBleLlFrame(uint8_t* out, const uint8_t* addr, uint8_t addrType, uint8_t pduType,
                         const uint8_t* adv, uint16_t advLen) {
    // The legacy advertising header's length field is 6 bits and covers AdvA + AdvData, so AdvData tops
    // out at 0x3F-6 = 57. Truncate rather than mask: a masked over-long length wraps to a small bogus
    // value and Wireshark then mis-dissects the whole record.
    const bool direct = (pduType == LL_ADV_DIRECT_IND);
    uint16_t payload = direct ? 0 : advLen;
    if (payload > 57) payload = 57;
    uint8_t* p = out;
    putLE32(p, kAdvAccessAddr); p += 4;
    uint8_t hdr0 = pduType & 0x0F;
    if (addrType != BLE_ADDR_PUBLIC) hdr0 |= 0x40;          // TxAdd: AdvA is a random address
    // RxAdd is left clear: the HCI report never carries TargetA's address type, and guessing it would
    // write a fabricated fact into the capture.
    *p++ = hdr0;
    *p++ = static_cast<uint8_t>((6 + (direct ? 6 : 0) + payload) & 0x3F);
    memcpy(p, addr, 6); p += 6;                              // AdvA, little-endian on air
    if (direct) { memset(p, 0, 6); p += 6; }                 // TargetA is not surfaced: zeroed, but present
    if (payload) { memcpy(p, adv, payload); p += payload; }
    *p++ = 0; *p++ = 0; *p++ = 0;                            // CRC we do not have (flags say "not checked")
    return static_cast<uint16_t>(p - out);
}

void trackBleDevice(const uint8_t* mac, int8_t rssi, uint8_t addrType, const AdInfo& ad, uint8_t bflags) {
    const uint32_t now = millis();
    bool isNew = false;
    portENTER_CRITICAL(&g_devMux);
    const int i = devFindSlot(bleDevs, kBleDevSlots, mac);
    BleDev& d = bleDevs[i];
    if (d.lastMs == 0 || !macEq(d.mac, mac)) {
        memset(&d, 0, sizeof(d));
        memcpy(d.mac, mac, 6);
        d.maxRssi = rssi;
        d.txPower = 127;
        d.surv = survLookup(mac);
        isNew = true;
        eventFlag(mac, rssi, 0, d.surv, EVF_BLE | (addrType ? EVF_RANDOM : 0));   // C4: random BLE addresses rotate
    }
    d.rssi = rssi;
    if (rssi > d.maxRssi) d.maxRssi = rssi;
    if (d.adv < 65535) d.adv++;
    d.lastMs = now;
    d.addrType = addrType;
    if (ad.haveName && ad.name[0]) strncpy(d.name, ad.name, sizeof(d.name));
    if (ad.company) d.company = ad.company;
    if (ad.appleType) d.appleType = ad.appleType;
    if (ad.appearance) d.appearance = ad.appearance;
    if (ad.txPower != 127) d.txPower = ad.txPower;
    if (ad.svc) d.svc = ad.svc;
    if (ad.svcData) d.svcData = ad.svcData;
    d.flags |= bflags;
    // kind == 0: a 15.4 key hunt (kind 1) must not match on a stale hunt.mac left by an earlier MAC hunt.
    noteHuntHit(hunt.kind == 0 && macEq(hunt.mac, mac), rssi, now);
    const bool named = d.name[0] != 0;
    portEXIT_CRITICAL(&g_devMux);
    // Only a scannable advertiser can answer a SCAN_REQ, so asking for an active window for anything else
    // would transmit for nothing. Flag only; the actual switch happens in serviceBle() on the loop task,
    // because restarting discovery from inside the GAP callback would re-enter the host.
    if (isNew && !named && (bflags & 4)) bleScan.activeUntilMs = now + kBleActiveWindowMs;
}

// One on-air advertising report. Runs in the NimBLE host task.
int bleGapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc& d = event->disc;
        const int8_t rssi = static_cast<int8_t>(d.rssi);
        AdInfo ad;
        parseAdStructures(d.data, d.length_data, ad);
        const uint8_t pdu = llPduTypeFromHci(d.event_type);
        const uint8_t bflags = ((pdu == LL_ADV_IND || pdu == LL_ADV_DIRECT_IND) ? 1 : 0)   // connectable
                             | 2                                                            // legacy adv
                             | ((pdu == LL_ADV_IND || pdu == LL_ADV_SCAN_IND) ? 4 : 0);     // scannable
        // ble_addr_t.val is little-endian (as it goes on air). Everything that displays or keys on a MAC
        // - the device table, the LCD, the dashboard, OUI lookup, the randomized-address bit, hunt
        // matching - expects the conventional MSB-first order, so reverse it here. The pcap builder below
        // deliberately keeps the raw little-endian bytes, because that is what the wire format wants.
        uint8_t mac[6];
        for (int i = 0; i < 6; i++) mac[i] = d.addr.val[5 - i];
        trackBleDevice(mac, rssi, d.addr.type, ad, bflags);
        bleScan.advSeen = bleScan.advSeen + 1;

        uint8_t nh;                                  // same single-producer discipline as the Wi-Fi path
        CapFrame* slot = capReserve(nh);
        if (slot) {
            const uint16_t n = buildBleLlFrame(slot->data, d.addr.val, d.addr.type, pdu,
                                               d.data, d.length_data);
            slot->ts_us = micros();
            slot->len = n;
            slot->capLen = n > capSnapLen ? capSnapLen : n;
            slot->rssi = rssi;
            slot->channel = 39;   // the HCI report does not say which of 37/38/39 it arrived on
            capCommit(nh);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        bleScan.scanDone = true;
    }
    return 0;
}

} // namespace

void startBle() {
    if (!bleScan.inited) {
        BLEDevice::init("bandwatch");   // brings up the NimBLE host; we drive discovery ourselves
        bleScan.inited = true;
    }
    struct ble_gap_disc_params p = {};
    p.itvl = 160;            // 100 ms in 0.625 ms units
    p.window = 128;          // 80 ms
    p.passive = bleScan.active ? 0 : 1;   // bleScan.active = what we decided to run
    p.filter_duplicates = 0; // every advertisement, not just the first from each address
    p.limited = 0;
    bleScan.scanDone = false;
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, bleGapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY && serialRoom(120))
        Serial.printf("{\"t\":\"err\",\"msg\":\"ble_gap_disc failed rc=%d\"}\n", rc);
}

void stopBle() {
    if (!bleScan.inited) return;
    ble_gap_disc_cancel();
    // deinit(false), not (true): core 3.3.11's BLEDevice::init() refuses once btMemReleased(BT_MODE_BLE), so
    // releasing would make "band ble" a one-shot. It is not a RAM cost: nimble_port_deinit() runs either
    // way and hands back the host task + msys pools; measured retention is ~260 B after the first visit,
    // ~0 after that (RAM audit, 2026-10). Re-check if the C5 BT config ever grows.
    BLEDevice::deinit(false);
    bleScan.inited = false;
}

// Discovery runs continuously (BLE_HS_FOREVER) and keeps no result cache, so there is nothing to recycle.
// This restarts it if the host ever ends discovery, and applies the passive/active policy.
void serviceBle() {
    if (bandMode != BAND_BLE || !bleScan.inited) return;
    if (bleScan.scanDone) {
        bleScan.cycles++;
        bleScan.scanDone = false;
        startBle();
        return;
    }
    // A capture must not be half passive and half active: whatever is running when recording starts stays.
    if (capActive) return;
    bool want = bleScan.active;
    switch (bleScan.mode) {
        case BLE_SCAN_PASSIVE: want = false; break;
        case BLE_SCAN_ACTIVE:  want = true;  break;
        case BLE_SCAN_AUTO:    want = static_cast<int32_t>(bleScan.activeUntilMs - millis()) > 0; break;
    }
    if (want == bleScan.active) return;
    const uint32_t now = millis();
    if (now - bleScan.lastSwitchMs < kBleSwitchMinMs) return;   // rate limit: restarting discovery costs a gap
    bleScan.lastSwitchMs = now;
    bleScan.switches++;
    bleScan.active = want;
    ble_gap_disc_cancel();
    startBle();
}
