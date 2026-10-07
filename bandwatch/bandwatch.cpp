#include "bandwatch.h"
#include "bandwatch_core.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <math.h>
#include <string.h>
#include <sdkconfig.h>
#include <esp_system.h>
#include <esp_ieee802154.h>   // channel control sets the 15.4 radio's channel directly

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

// Wall clock: the host sends "time <epoch>"; without it timestamps fall back to uptime (1970-based).
uint32_t epochBase = 0;         // epoch seconds at millis() == epochBaseMs
uint32_t epochBaseMs = 0;
bool epochValid = false;

volatile uint8_t currentChannelNum = 0;
BandMode bandMode = BAND_5G;

int enabledCount() {
    int n = 0;
    for (int i = 0; i < kChannelCount; i++) if (chanEnabled(i)) n++;
    return n;
}

ChannelState channels[kChannelCount];
uint8_t specStepMhz = kSpecStepDefault;   // fine-spectrum step; in spec mode currentIdx indexes specFine[]
SpecBin specFine[kSpecMaxBins];
int currentSpecMhz = 0;
int currentIdx = 0;                 // Index into kChannels (in spec mode: index into specFine[])
int parkedIdx = -1;                 // >= 0: stay on this channel instead of hopping
uint32_t dwellStartedMs = 0;
uint32_t sweepCount = 0;
bool monitorReady = false;
bool wifiRunning = false;
esp_err_t errCountry = ESP_FAIL, errBand = ESP_FAIL, errProto = ESP_FAIL, errPromisc = ESP_FAIL;

// File-local helper for computeBusyScore().
inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void resetAccum() {
    portENTER_CRITICAL(&g_accumMux);
    g_accum.frames = 0;
    g_accum.bytes = 0;
    g_accum.strong = 0;
    g_accum.unique = 0;
    g_accum.macFill = 0;
    for (int i = 0; i < kUniqueSlots; i++) g_accum.macHashes[i] = 0;
    g_accum.bestRssi = -128;
    for (int i = 0; i < 6; i++) g_accum.bestMac[i] = 0;
    portEXIT_CRITICAL(&g_accumMux);
}

// ---------------------------------------------------------------------------------------------
// Channel control (the radio lifecycle halves live in wifi_sniff.cpp and ieee154.cpp)
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
    if (!hopActive()) return false;
    if (modeSpec()) {   // fine spectrum: step off the channel grid in specStepMhz increments across 2.4 GHz
        if (parkedIdx >= 0 && chanEnabled(parkedIdx)) {
            // Parked (spec only enables the 15.4 set): hold the bin nearest that channel's centre. The sweep
            // never wraps while parked, so sweepCount and the full-sweep "fs" line pause; "fd" keeps coming.
            currentIdx = (ch154Freq(kChannels[parkedIdx]) - kSpecLoMhz + specStepMhz / 2) / specStepMhz;
            if (currentIdx >= specBinCount()) currentIdx = specBinCount() - 1;
        } else {
            currentIdx += 1;
            if (currentIdx < 0 || currentIdx >= specBinCount()) currentIdx = 0;
        }
        currentSpecMhz = specBinMhz(currentIdx);
        edSetFreqMhz(currentSpecMhz);
        edReset();
        edKick();
        currentChannelNum = 0;
        dwellStartedMs = millis();
        return true;
    }
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

void resetChannelStats() {
    for (int i = 0; i < kChannelCount; i++) {
        channels[i].hasData = false;
        channels[i].busyEma = channels[i].busyCurrent = 0.0f;
        channels[i].metrics = ChannelMetrics{};
    }
    for (int i = 0; i < kSpecMaxBins; i++) specFine[i] = SpecBin{};
    sweepCount = 0;
    resetAccum();
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
    const bool prevRadio = (prev == BAND_BLE) || (prev == BAND_154) || (prev == BAND_SPEC);
    const bool newRadio = (m == BAND_BLE) || (m == BAND_154) || (m == BAND_SPEC);
    if (prevRadio || newRadio) {
        if (prev == BAND_BLE) stopBle();
        else if (prev == BAND_154) stop154();
        else if (prev == BAND_SPEC) stopSpectrum();
        else stopWifi();
        if (m == BAND_BLE) { startBle(); return; }
        if (m == BAND_154 || m == BAND_SPEC) {
            if (m == BAND_154) start154();
            else startSpectrum();
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

namespace {   // file-local scoring helper (the snapshot fns below stay exported)

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

// C8: the quietest channel with real data (lowest busyEma among enabled, swept, non-rejected channels).
// Returns -1 if nothing has been measured yet. The caller decides whether to caveat an all-busy set.
int quietestChannel() {
    int best = -1;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i) || !channels[i].hasData || channels[i].unavailable) continue;
        if (best < 0 || channels[i].busyEma < channels[best].busyEma) best = i;
    }
    return best;
}

// Device-table listing helpers (loop task only - UI timer and host output both run there). collect*Refs()
// copies a compact {rssi, slot, identity} per fresh device under the lock and sorts those; fetch*Dev() then
// re-validates each slot and copies out one full record. See the DevRef note in bandwatch_core.h.
DevRef g_devRefs[kWifiDevSlots];

// Collect compact refs to fresh devices under the lock, then sort by RSSI outside it. The full record is
// re-fetched later with fetch*Dev(); only {rssi, slot, identity} is copied here, so the lock is held briefly
// and no large scratch is needed.
int collectWifiRefs(DevRef* refs, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kWifiDevSlots && n < maxN; i++) {
        if (devFresh(wifiDevs[i].lastMs, now, freshMs)) {
            // Sort key only. A tier-1 (destination-only) sighting has never been heard, so it carries rssi 0;
            // left as is it would sort above every real transmitter and push them off the LCD's 12 rows.
            refs[n].rssi = (wifiDevs[i].flags & 4) ? INT8_MIN : wifiDevs[i].rssi;
            refs[n].idx = static_cast<uint8_t>(i);
            memcpy(refs[n].key, wifiDevs[i].mac, 6);
            n++;
        }
    }
    portEXIT_CRITICAL(&g_devMux);
    sortByRssi(refs, n);
    return n;
}
int collectBleRefs(DevRef* refs, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kBleDevSlots && n < maxN; i++) {
        if (devFresh(bleDevs[i].lastMs, now, freshMs)) {
            refs[n].rssi = bleDevs[i].rssi;
            refs[n].idx = static_cast<uint8_t>(i);
            memcpy(refs[n].key, bleDevs[i].mac, 6);
            n++;
        }
    }
    portEXIT_CRITICAL(&g_devMux);
    sortByRssi(refs, n);
    return n;
}
int collect154Refs(DevRef* refs, int maxN, uint32_t freshMs) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kDev154Slots && n < maxN; i++) {
        if (devFresh(devs154[i].lastMs, now, freshMs)) {
            refs[n].rssi = devs154[i].rssi;
            refs[n].idx = static_cast<uint8_t>(i);
            memcpy(refs[n].key, devs154[i].key, 8);
            n++;
        }
    }
    portEXIT_CRITICAL(&g_devMux);
    sortByRssi(refs, n);
    return n;
}

// Re-fetch one full record for a ref, under a brief lock. Returns false (skip the row) if the slot is no
// longer fresh or now holds a different device than the ref named — i.e. it was evicted/reused since collect.
bool fetchWifiDev(const DevRef& r, WifiDev& out, uint32_t freshMs) {
    const uint32_t now = millis();
    bool ok = false;
    portENTER_CRITICAL(&g_devMux);
    const WifiDev& d = wifiDevs[r.idx];
    if (devFresh(d.lastMs, now, freshMs) && macEq(d.mac, r.key)) { out = d; ok = true; }
    portEXIT_CRITICAL(&g_devMux);
    return ok;
}
bool fetchBleDev(const DevRef& r, BleDev& out, uint32_t freshMs) {
    const uint32_t now = millis();
    bool ok = false;
    portENTER_CRITICAL(&g_devMux);
    const BleDev& d = bleDevs[r.idx];
    if (devFresh(d.lastMs, now, freshMs) && macEq(d.mac, r.key)) { out = d; ok = true; }
    portEXIT_CRITICAL(&g_devMux);
    return ok;
}
bool fetch154Dev(const DevRef& r, Dev154& out, uint32_t freshMs) {
    const uint32_t now = millis();
    bool ok = false;
    portENTER_CRITICAL(&g_devMux);
    const Dev154& d = devs154[r.idx];
    if (devFresh(d.lastMs, now, freshMs) && key8Eq(d.key, r.key)) { out = d; ok = true; }
    portEXIT_CRITICAL(&g_devMux);
    return ok;
}

void setPark(int idx) {
    parkedIdx = idx;
    if (parkedIdx >= 0 && hopMode()) monitorReady = advanceChannel();
}

void lookupHuntLabel() {
    hunt.label[0] = 0;
    if (hunt.kind == 1) {
        // Copy the two fields under the lock and format after it: the radio callbacks wait on g_devMux.
        bool found = false;
        uint8_t proto = 0;
        uint16_t pan = 0;
        portENTER_CRITICAL(&g_devMux);
        for (int i = 0; i < kDev154Slots; i++)
            if (devs154[i].lastMs && key8Eq(devs154[i].key, hunt.key)) { found = true; proto = devs154[i].proto; pan = devs154[i].pan; break; }
        portEXIT_CRITICAL(&g_devMux);
        if (found) {
            static const char* const kProto[] = {"802.15.4", "Zigbee", "Zigbee GP", "Thread", "MAC-secured"};
            snprintf(hunt.label, sizeof(hunt.label), "%s pan %04x", kProto[proto < 5 ? proto : 0], pan);
        }
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

// ---------------------------------------------------------------------------------------------
// Dwell / hop
// ---------------------------------------------------------------------------------------------
void finishDwell() {
    if (modeSpec()) {   // fine spectrum: store this frequency bin's energy; currentIdx indexes specFine[]
        int8_t mn, mx, mean; uint16_t n;
        edSnapshot(mn, mx, mean, n);
        if (currentIdx >= 0 && currentIdx < specBinCount()) specFine[currentIdx] = SpecBin{mn, mean, mx, n};
        sendDwell(currentIdx);
        return;
    }
    ChannelMetrics snap{};
    portENTER_CRITICAL(&g_accumMux);
    snap.frames = g_accum.frames;
    snap.bytes = g_accum.bytes;
    snap.strong = g_accum.strong;
    snap.unique = g_accum.unique;
    portEXIT_CRITICAL(&g_accumMux);

    ChannelState& ch = channels[currentIdx];   // spec mode returned above; here currentIdx indexes channels[]
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
    if (!hopActive()) return;
    const uint32_t now = millis();
    const uint32_t dwell = dwellMs();
    if (!monitorReady) {
        if ((now - dwellStartedMs) < dwell) return;
        dwellStartedMs = now;
        monitorReady = advanceChannel();
        return;
    }
    if ((now - dwellStartedMs) < dwell) return;

    finishDwell();
    const int lastIdx = currentIdx;
    monitorReady = advanceChannel();
    // Reset after the switch, not before it: frames that landed between the two used to be counted toward the
    // next channel. (A frame from the old channel still in flight is now dropped instead, which is the safer error.)
    resetAccum();
    if (monitorReady && parkedIdx < 0 && currentIdx < lastIdx) {
        sweepCount += 1;
        sendSweep();
    }
}

void Bandwatch_Init(void) {
    pinMode(kBootButtonPin, INPUT_PULLUP);   // the device tables are BSS: already zero
    // Probe the card at boot so "hello" can tell the dashboard whether SD recording is available.
    // SPI is already up: LCD_Init() runs before this, and both share the bus from the loop task.
    sdProbeAtBoot();
    loadSettings();   // restore band mode + scalar policies before the radios come up (C3)
    if (g_eventsWanted) eventsEnable();   // C4, persisted: armed even with no card (it retries)

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
    startWifi();   // brings up BAND_5G (the compile default); a restored non-5G mode transitions below
    if (g_restoredMode != BAND_5G) setBandMode(g_restoredMode);   // reuse the tested teardown/bringup path
    if (g_settingsRestored)
        Serial.printf("{\"t\":\"log\",\"msg\":\"settings restored: band %s\"}\n", kBandName[bandMode]);
    Serial.printf("{\"t\":\"log\",\"msg\":\"boot: heap after wifi %u\"}\n", static_cast<unsigned>(ESP.getFreeHeap()));
    sendHello();
    refreshUi();
}

void Bandwatch_Loop(void) {
    static uint32_t lastDevMs = 0, bleStatusMs = 0;
    pollSerial();
    pollButton();
    serviceMirror();     // paces the live LCD-mirror full refresh (no-op unless mirror is on)
    serviceSdRead();
    drainCapture();
    serviceProbes();     // C1: directed probe requests -> "pr" lines (Wi-Fi modes)
    sdServicePresence(); // card-presence probe while idle (LCD face on removal/insertion)
    serviceSdFace();
    serviceEvents();     // C4: classify new/surveillance sightings, flush /events.csv (no-op unless events on)
    sendEventStatus();
    serviceLedAlerts();  // D1: surveillance / permit-join / new-device LED blips (yields to deauth)
    serviceBle();
    serviceSpectrum();   // re-arm energy detection (spec mode only; no-op otherwise)
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
