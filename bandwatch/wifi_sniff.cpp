// Wi-Fi promiscuous sniffing for Bandwatch: the driver lifecycle (startWifi/stopWifi) and the RX path that runs
// in the Wi-Fi task - everything on that side is IRAM_ATTR and spinlock-guarded, no heap, no Serial. The TX-power
// and raw-rate setup in startWifi is what makes the deauth attack's hand-rolled frames work (deauth_diag.cpp).
#include "bandwatch_core.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include "surv_ouis.h"   // surveillance-OUI table (matched in firmware so the LCD can flag without a host)

ProbeEvt g_probeQ[kProbeQ];
volatile uint8_t probeHead = 0, probeTail = 0;
volatile uint32_t probeDropped = 0;

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

inline uint16_t macHash(const uint8_t* mac) {
    return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
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
                               uint16_t sigLen, bool destOnly = false, const uint8_t* bssid = nullptr) {
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
        eventFlag(mac, rssi, currentChannelNum, d.surv, destOnly ? EVF_TIER1 : 0);   // C4: no-op unless events on
        if (d.surv) ledNoteSurv(mac);   // 1.6.1 LED alert, independent of the event log
    }
    if (!destOnly) d.flags &= ~4;   // heard it transmit: upgrade to tier 2
    // Association, when the DS bits made it unambiguous. Never cleared once learned: a station that goes
    // quiet is still on that BSS, and a roam overwrites it on the next frame. Skipped when the BSSID is
    // this device itself, which is just an AP talking on its own BSS.
    if (bssid && !macEq(bssid, mac)) { d.apSuffix[0] = bssid[3]; d.apSuffix[1] = bssid[4]; d.apSuffix[2] = bssid[5]; }
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
    // kind == 0: a 15.4 key hunt (kind 1) must not match on a stale hunt.mac left by an earlier MAC hunt.
    noteHuntHit(hunt.kind == 0 && macEq(hunt.mac, mac), rssi, now);
    portEXIT_CRITICAL_ISR(&g_devMux);
}

// Directed probe request -> g_probeQ. Body starts at 24 (no fixed fields, unlike a beacon's 36); the first IE
// is the SSID, and a zero-length one is a wildcard probe that names nothing, so it is skipped. Frame check
// sequence is the last 4 bytes of sig_len. Runs on the Wi-Fi task: copy and go, no Serial, no heap.
void IRAM_ATTR queueProbe(const uint8_t* p, uint16_t sigLen, int8_t rssi) {
    if (sigLen < 24 + 2 + 4) return;
    const uint8_t tag = p[24], len = p[25];
    if (tag != 0 || len == 0 || len > 32 || 26 + len > sigLen - 4) return;
    const uint8_t head = probeHead;
    const uint8_t next = static_cast<uint8_t>((head + 1) % kProbeQ);
    if (next == probeTail) { probeDropped = probeDropped + 1; return; }
    ProbeEvt& e = g_probeQ[head];
    memcpy(e.mac, p + 10, 6);   // addr2: the probing client
    e.rssi = rssi;
    e.ch = static_cast<uint8_t>(currentChannelNum);
    memcpy(e.ssid, p + 26, len);
    e.ssid[len] = 0;
    sanitizeText(e.ssid, sizeof(e.ssid));   // rule 9: strings off the air are hostile
    probeHead = next;
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
        if (pkt->rx_ctrl.rssi > g_accum.bestRssi) {   // C6: loudest frame this dwell names its transmitter
            g_accum.bestRssi = pkt->rx_ctrl.rssi;
            for (int i = 0; i < 6; i++) g_accum.bestMac[i] = src[i];
        }
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
        // Which address is the BSSID depends on the DS bits (802.11-2020 9.3.2.1, table 9-26). Only the two
        // unambiguous single-hop cases are used; ToDS+FromDS (WDS/mesh) has no station-to-BSS meaning here,
        // and ToDS=FromDS=0 puts the BSSID in addr3, which is the AP's own address for the beacons that
        // dominate that case. txBssid = the transmitter's BSS, rxBssid = the addr1 device's BSS.
        const uint8_t fc1 = pkt->payload[1];
        const bool toDs = fc1 & 0x01, fromDs = fc1 & 0x02;
        const uint8_t* txBssid = (toDs && !fromDs) ? ipkt->hdr.addr1 : nullptr;   // station -> AP: addr1 is the BSSID
        const uint8_t* rxBssid = (!toDs && fromDs) ? ipkt->hdr.addr2 : nullptr;   // AP -> station: addr2 is the BSSID
        trackWifiDevice(ipkt->hdr.addr2, pkt->rx_ctrl.rssi, pkt->payload[0], pkt->payload, sigLen, false, txBssid);
        // Receiver-side sighting: a device that never transmits during our dwell is still named as addr1
        // by whoever talks to it. Unicast only - broadcast/multicast destinations are not devices.
        if (trackAddr1 && !(ipkt->hdr.addr1[0] & 0x01) && !macEq(ipkt->hdr.addr1, ipkt->hdr.addr2))
            trackWifiDevice(ipkt->hdr.addr1, pkt->rx_ctrl.rssi, 0, pkt->payload, sigLen, true, rxBssid);
    }

    if (type == WIFI_PKT_MGMT && pkt->payload[0] == 0x40) queueProbe(pkt->payload, sigLen, pkt->rx_ctrl.rssi);   // C1

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
