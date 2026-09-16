#include "bandwatch.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <math.h>
#include <string.h>
#include <sdkconfig.h>

#if !CONFIG_SOC_WIFI_SUPPORT_5G
#error "Bandwatch 5G needs a 5 GHz capable target (ESP32-C5). Select 'ESP32C5 Dev Module'."
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kDwellMs = 220;          // Dwell per channel (200–400 ms)
constexpr uint32_t kUiIntervalMs = 120;     // UI refresh cadence
constexpr int kStrongThresholdDbm = -65;    // "Strong" frame threshold
constexpr float kBusyEmaAlpha = 0.22f;      // Smoothing within required 0.15–0.30
constexpr int kUniqueSlots = 24;            // Best-effort unique transmitter slots
constexpr int kRgbPin = 8;                  // Onboard WS2812B data pin (Waveshare ESP32-C5-LCD-1.47)
constexpr int kBootButtonPin = 28;          // BOOT key = GPIO28 strap; free to use as an input after boot
constexpr uint32_t kLongPressMs = 700;      // Hold BOOT this long to park/unpark on the busiest channel
constexpr uint32_t kApUpdateMs = 3000;      // AP count refresh cadence
constexpr const char* kCountryCode = "EU";  // Only affects the regulatory table; we never transmit.

// Channels to sweep. The C5 has ONE radio, so bands are time-shared: a "both" sweep simply
// interleaves 2.4 GHz channels 1-13 with the 5 GHz list below (38 dwells, ~8.4 s per sweep).
// 5 GHz: UNII-1 (36–48), UNII-2A (52–64, DFS), UNII-2C (100–144, DFS), UNII-3 (149–165).
// Receiving on DFS channels is passive; the radio never transmits in promiscuous mode.
// Channels the driver refuses (ESP_ERR_INVALID_ARG) are skipped automatically.
enum BandMode : uint8_t { BAND_5G = 0, BAND_24G = 1, BAND_BOTH = 2 };
constexpr const char* kBandName[] = {"5g", "2.4g", "both"};
constexpr uint8_t kChannels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
constexpr int kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);
constexpr int kFirst5gIdx = 13;
inline bool is5g(int idx) { return idx >= kFirst5gIdx; }
// Group boundaries (index of first channel of each group) for the spectrum strip spacing.
constexpr int kGroupStart[] = {0, 13, 21, 33};

// Allow every 5 GHz channel the driver knows about (bits 1..28, see wifi_5g_channel_bit_t).
constexpr uint32_t kAll5gChannelMask = 0x1FFFFFFEu;

// Frame capture (streamed to the host over USB serial as base64 lines, host writes the pcap).
constexpr int kCapSlots = 20;
constexpr uint16_t kCapMaxLen = 1600;

constexpr int kPageCount = 3;
constexpr const char* kVersion = "1.0";

// ---------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------
inline lv_color_t c565(uint16_t v) {
    const uint8_t r5 = (v >> 11) & 0x1F;
    const uint8_t g6 = (v >> 5) & 0x3F;
    const uint8_t b5 = v & 0x1F;
    const uint8_t r8 = (uint16_t(r5) * 255) / 31;
    const uint8_t g8 = (uint16_t(g6) * 255) / 63;
    const uint8_t b8 = (uint16_t(b5) * 255) / 31;
    return lv_color_make(r8, g8, b8);
}

constexpr uint16_t BG_565      = 0x0122; // deep blue/black
constexpr uint16_t PANEL_565   = 0x0843; // muted navy
constexpr uint16_t WHITE_565   = 0xFFFF;
constexpr uint16_t BLACK_565   = 0x0000;
constexpr uint16_t GREEN_565   = 0x07E0;
constexpr uint16_t RED_565     = 0xF800;
constexpr uint16_t CYAN_565    = 0x07FF;
constexpr uint16_t GREY_565    = 0x8410;
constexpr uint16_t DIM_565     = 0x2124;
constexpr uint16_t YELLOW_565  = 0xFFE0;

struct RgbColor { uint8_t r; uint8_t g; uint8_t b; };
constexpr RgbColor LED_GREEN  = {0, 180, 40};
constexpr RgbColor LED_YELLOW = {255, 200, 0};
constexpr RgbColor LED_ORANGE = {255, 120, 0};
constexpr RgbColor LED_RED    = {255, 24, 0};

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
struct Accum {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint16_t strong = 0;
    uint16_t unique = 0;
    uint16_t macHashes[kUniqueSlots] = {0};
    uint8_t macFill = 0;
};

struct ChannelMetrics {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint16_t strong = 0;
    uint16_t unique = 0;
};

struct ChannelState {
    ChannelMetrics metrics;
    float busyCurrent = 0.0f;  // Last dwell busy score (0–100)
    float busyEma = 0.0f;      // Smoothed busy score (0–100)
    bool hasData = false;
    bool unavailable = false;  // Driver rejected esp_wifi_set_channel for this channel
};

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

struct CapFrame {
    uint32_t ts_us;
    uint16_t len;      // original MPDU length (sig_len)
    uint16_t capLen;   // bytes stored
    int8_t rssi;
    uint8_t channel;
    uint8_t data[kCapMaxLen];
};

volatile Accum g_accum;
portMUX_TYPE g_accumMux = portMUX_INITIALIZER_UNLOCKED;

// Single-producer (Wi-Fi task) / single-consumer (loop) ring buffer for captured frames.
CapFrame capRing[kCapSlots];
volatile uint8_t capHead = 0;     // next slot the producer writes
volatile uint8_t capTail = 0;     // next slot the consumer reads
volatile bool captureEnabled = false;
volatile uint16_t capSnapLen = kCapMaxLen;
volatile uint32_t capDropped = 0;
uint32_t capSent = 0;
volatile uint8_t currentChannelNum = 0;
BandMode bandMode = BAND_5G;

inline bool chanEnabled(int idx) {
    if (bandMode == BAND_BOTH) return true;
    return (bandMode == BAND_5G) == is5g(idx);
}
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
esp_err_t errCountry = ESP_FAIL, errBand = ESP_FAIL, errProto = ESP_FAIL, errPromisc = ESP_FAIL;

// UI objects
lv_obj_t* pages[kPageCount] = {nullptr};
int currentPage = 0;
// page 0: overview
lv_obj_t* chanLabel = nullptr;
lv_obj_t* globalBar = nullptr;
lv_obj_t* globalLabel = nullptr;
lv_obj_t* sweepLabel = nullptr;
lv_obj_t* topRows[3] = {nullptr};
lv_obj_t* topRates[3] = {nullptr};
lv_obj_t* topBars[3] = {nullptr};
lv_obj_t* specBars[kChannelCount] = {nullptr};
lv_obj_t* specLabel = nullptr;
lv_obj_t* statsLine1 = nullptr;
lv_obj_t* statsLine2 = nullptr;
lv_obj_t* footLabel = nullptr;
// page 1: all channels
lv_obj_t* listChanLabel = nullptr;
constexpr int kListSlots = 39;   // 3 columns x 13 rows
lv_obj_t* listRow[kListSlots] = {nullptr};
lv_obj_t* listName[kListSlots] = {nullptr};
lv_obj_t* listBar[kListSlots] = {nullptr};
lv_obj_t* listVal[kListSlots] = {nullptr};
lv_obj_t* listFoot = nullptr;
// page 2: system
constexpr int kSysLines = 13;
lv_obj_t* sysLines[kSysLines] = {nullptr};

uint16_t lastApSeen = 0;
uint16_t apMaxWindow = 0;
uint32_t apWindowStartedMs = 0;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
inline void setLedColor(const RgbColor& c, uint8_t brightness = 60) {
    const uint16_t scale = static_cast<uint16_t>(brightness) * 255 / 100;
    const uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(c.r) * scale) / 255);
    const uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(c.g) * scale) / 255);
    const uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(c.b) * scale) / 255);
    rgbLedWrite(kRgbPin, r, g, b);  // Arduino-ESP32 built-in WS2812 driver (RMT), GRB order
}

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline uint16_t macHash(const uint8_t* mac) {
    // Lightweight hash using last two bytes to avoid heavy math in the RX path.
    return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
}

lv_color_t scoreColor(float s) {
    if (s > 70.0f) return c565(RED_565);
    if (s > 40.0f) return c565(YELLOW_565);
    return c565(GREEN_565);
}

void fmtRate(char* out, size_t n, float perSec, const char* unit) {
    if (perSec >= 10000.0f) snprintf(out, n, "%.0fk%s", perSec / 1000.0f, unit);
    else if (perSec >= 1000.0f) snprintf(out, n, "%.1fk%s", perSec / 1000.0f, unit);
    else snprintf(out, n, "%.0f%s", perSec, unit);
}

// ---------------------------------------------------------------------------------------------
// Promiscuous RX path (runs in the Wi-Fi task: count, hash, optionally copy – nothing else)
// ---------------------------------------------------------------------------------------------
void IRAM_ATTR promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) return;
    const wifi_promiscuous_pkt_t* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->rx_ctrl.rx_state != 0) return;  // CRC/PHY error: not real airtime we can attribute
    // Control frames can be as short as 10 bytes (ACK/CTS: FC+duration+addr1); everything else has
    // at least addr1..addr3 + seq (24 bytes). addr2 (transmitter) exists only from 16 bytes on.
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
            if (g_accum.macHashes[i] == h) {
                known = true;
                break;
            }
        }
        if (!known && g_accum.macFill < kUniqueSlots) {
            const uint8_t slot = g_accum.macFill;
            g_accum.macHashes[slot] = h;
            g_accum.macFill = slot + 1;
            g_accum.unique += 1;
        }
    }
    portEXIT_CRITICAL_ISR(&g_accumMux);

    if (captureEnabled) {
        const uint8_t head = capHead;
        const uint8_t next = (head + 1) % kCapSlots;
        if (next == capTail) {
            capDropped = capDropped + 1;   // consumer too slow: drop, but count it
            return;
        }
        CapFrame& f = capRing[head];
        uint16_t n = sigLen;
        if (n > capSnapLen) n = capSnapLen;
        if (n > kCapMaxLen) n = kCapMaxLen;
        f.ts_us = pkt->rx_ctrl.timestamp;
        f.len = sigLen;
        f.capLen = n;
        f.rssi = pkt->rx_ctrl.rssi;
        f.channel = currentChannelNum;
        memcpy(f.data, pkt->payload, n);
        capHead = next;
    }
}

void resetAccum() {
    portENTER_CRITICAL(&g_accumMux);
    g_accum.frames = 0;
    g_accum.bytes = 0;
    g_accum.strong = 0;
    g_accum.unique = 0;
    g_accum.macFill = 0;
    for (int i = 0; i < kUniqueSlots; i++) {
        g_accum.macHashes[i] = 0;
    }
    portEXIT_CRITICAL(&g_accumMux);
}

// ---------------------------------------------------------------------------------------------
// Channel control
// ---------------------------------------------------------------------------------------------
// Returns true if the radio is now parked on kChannels[idx].
bool applyChannelIdx(int idx) {
    // In the 5 GHz band the secondary channel is derived from the primary by the driver.
    const esp_err_t err = esp_wifi_set_channel(kChannels[idx], WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        Serial.printf("{\"t\":\"log\",\"msg\":\"channel %u rejected (%s)%s\"}\n", kChannels[idx],
                      esp_err_to_name(err), err == ESP_ERR_INVALID_ARG ? " - skipping permanently" : " - will retry");
        // Only a rejected argument is permanent (regulatory/unsupported); anything else is transient
        // (e.g. the driver is still starting) and gets retried on the next hop.
        if (err == ESP_ERR_INVALID_ARG) channels[idx].unavailable = true;
        return false;
    }
    currentChannelNum = kChannels[idx];
    dwellStartedMs = millis();
    return true;
}

// Advance to the next usable channel (skips ones the driver refused). Returns false if none work.
bool advanceChannel() {
    if (parkedIdx >= 0 && !channels[parkedIdx].unavailable) {
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

wifi_band_mode_t toDriverBand(BandMode m) {
    return m == BAND_5G ? WIFI_BAND_MODE_5G_ONLY : m == BAND_24G ? WIFI_BAND_MODE_2G_ONLY : WIFI_BAND_MODE_AUTO;
}

void applyProtocols() {
    // Enable every PHY mode so HE/VHT/HT frames are captured too.
    wifi_protocols_t protos = {};
    protos.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    protos.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
    errProto = esp_wifi_set_protocols(WIFI_IF_STA, &protos);
}

// Switch band mode at runtime: clears per-channel history and restarts the sweep.
void setBandMode(BandMode m) {
    bandMode = m;
    if (parkedIdx >= 0 && !chanEnabled(parkedIdx)) parkedIdx = -1;
    errBand = esp_wifi_set_band_mode(toDriverBand(m));
    delay(100);
    applyProtocols();
    for (int i = 0; i < kChannelCount; i++) {
        channels[i].hasData = false;
        channels[i].busyEma = channels[i].busyCurrent = 0.0f;
        channels[i].metrics = ChannelMetrics{};
    }
    sweepCount = 0;
    resetAccum();
    currentIdx = -1;
    monitorReady = advanceChannel();
}

int indexOfChannel(int ch) {
    for (int i = 0; i < kChannelCount; i++) if (kChannels[i] == ch) return i;
    return -1;
}

void ensureWifiMonitor() {
    static bool started = false;
    if (started) return;
    started = true;

    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);     // esp_wifi_init + esp_wifi_start
    WiFi.setSleep(false);

    // Manual policy + explicit 5 GHz mask so DFS channels are selectable for passive listening.
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
    delay(100);  // Arduino's own WiFi.setBandMode() settles for 100 ms after a band switch
    applyProtocols();

    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    errPromisc = esp_wifi_set_promiscuous(true);

    resetAccum();
    currentIdx = -1;
    monitorReady = advanceChannel();
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------
float computeBusyScore(const ChannelMetrics& m) {
    const float dwellSec = static_cast<float>(kDwellMs) / 1000.0f;
    const float pps = m.frames / dwellSec;           // packets per second
    const float bps = m.bytes / dwellSec;            // bytes per second
    const float strongRatio = (m.frames > 0) ? (static_cast<float>(m.strong) / static_cast<float>(m.frames)) : 0.0f;

    // Log-scaled terms to keep stability across quiet and busy environments.
    const float ppsScore = clamp01(log1pf(pps) / logf(600.0f));          // ~600 pps -> near 1
    const float bpsScore = clamp01(log1pf(bps) / logf(50000.0f));        // ~50 KB/s -> near 1
    const float uniqueScore = clamp01(log1pf(static_cast<float>(m.unique)) / logf(20.0f)); // soft cap ~20 talkers

    const float raw = 0.40f * ppsScore + 0.30f * bpsScore + 0.20f * strongRatio + 0.10f * uniqueScore;
    return clamp01(raw) * 100.0f;
}

float globalActivityMax() {
    float maxVal = 0.0f;
    for (int i = 0; i < kChannelCount; i++) {
        if (chanEnabled(i) && channels[i].hasData && channels[i].busyEma > maxVal) {
            maxVal = channels[i].busyEma;
        }
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

// ---------------------------------------------------------------------------------------------
// Host protocol (USB serial, one JSON object per line; frames as "P ..." base64 lines)
// ---------------------------------------------------------------------------------------------
// Non-blocking serial policy: the USB CDC TX buffer is large (see setup) and writes never block. To avoid
// half-written lines when the host is slow or absent, every line checks for room first and is dropped whole.
inline bool serialRoom(size_t n) { return static_cast<size_t>(Serial.availableForWrite()) >= n; }

void sendHello() {
    if (!serialRoom(600)) return;
    Serial.printf("{\"t\":\"hello\",\"fw\":\"bandwatch\",\"ver\":\"%s\",\"dwell_ms\":%u,\"band\":\"%s\",\"country\":\"%s\",\"bandmode\":\"%s\","
                  "\"proto\":\"%s\",\"promisc\":\"%s\",\"chs\":[",
                  kVersion, static_cast<unsigned>(kDwellMs), kBandName[bandMode], esp_err_to_name(errCountry), esp_err_to_name(errBand),
                  esp_err_to_name(errProto), esp_err_to_name(errPromisc));
    bool first = true;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        Serial.printf("%s%u", first ? "" : ",", kChannels[i]);
        first = false;
    }
    Serial.printf("],\"park\":%d,\"cap\":%d,\"snap\":%u,\"heap\":%u}\n",
                  parkedIdx >= 0 ? kChannels[parkedIdx] : 0, captureEnabled ? 1 : 0,
                  static_cast<unsigned>(capSnapLen), static_cast<unsigned>(ESP.getFreeHeap()));
}

// Emitted after every completed dwell.
void sendDwell(int idx) {
    if (!serialRoom(220)) return;
    const ChannelState& ch = channels[idx];
    Serial.printf("{\"t\":\"d\",\"c\":%u,\"s\":%.1f,\"r\":%.1f,\"f\":%lu,\"b\":%lu,\"st\":%u,\"u\":%u,"
                  "\"g\":%.1f,\"n\":%lu,\"park\":%d,\"cap\":%d,\"drop\":%lu}\n",
                  kChannels[idx], ch.busyEma, ch.busyCurrent,
                  static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                  ch.metrics.strong, ch.metrics.unique, globalActivityMax(),
                  static_cast<unsigned long>(sweepCount), parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped));
}

// Emitted after every full sweep: snapshot of every channel so a host can join at any time.
void sendSweep() {
    if (!serialRoom(1400)) return;
    Serial.printf("{\"t\":\"s\",\"n\":%lu,\"g\":%.1f,\"band\":\"%s\",\"ch\":[", static_cast<unsigned long>(sweepCount),
                  globalActivityMax(), kBandName[bandMode]);
    bool first = true;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        // [channel, smoothed score, frames, bytes, strong, unique, state]  state: 0 ok, 1 no data, 2 unavailable
        Serial.printf("%s[%u,%.1f,%lu,%lu,%u,%u,%d]", first ? "" : ",", kChannels[i], ch.busyEma,
                      static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                      ch.metrics.strong, ch.metrics.unique, ch.unavailable ? 2 : (ch.hasData ? 0 : 1));
        first = false;
    }
    Serial.printf("],\"aps\":%u,\"drop\":%lu,\"heap\":%u}\n", lastApSeen, static_cast<unsigned long>(capDropped),
                  static_cast<unsigned>(ESP.getFreeHeap()));
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void writeBase64(const uint8_t* d, size_t n) {
    char out[68];
    size_t i = 0;
    while (i < n) {
        size_t o = 0;
        const size_t chunkEnd = (i + 48 < n) ? i + 48 : n;   // 48 bytes -> 64 chars per write
        while (i + 2 < chunkEnd) {
            const uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
            out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
            out[o++] = kB64[(v >> 6) & 63];  out[o++] = kB64[v & 63];
            i += 3;
        }
        if (i < chunkEnd && chunkEnd == n) {   // tail (1 or 2 bytes)
            const size_t rem = n - i;
            const uint32_t v = (d[i] << 16) | (rem == 2 ? (d[i + 1] << 8) : 0);
            out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
            out[o++] = (rem == 2) ? kB64[(v >> 6) & 63] : '=';
            out[o++] = '=';
            i = n;
        }
        Serial.write(reinterpret_cast<uint8_t*>(out), o);
    }
}

// Drain captured frames to the host. Bounded per call so the UI stays responsive.
void drainCapture() {
    int budget = 8;
    while (capTail != capHead && budget-- > 0) {
        const CapFrame& f = capRing[capTail];
        // base64 (4/3) + header/newline; if the host is not draining fast enough, leave the frame queued.
        if (!serialRoom((f.capLen * 4) / 3 + 40)) return;
        Serial.printf("P %u %d %lu %u ", f.channel, f.rssi, static_cast<unsigned long>(f.ts_us), f.len);
        writeBase64(f.data, f.capLen);
        Serial.write('\n');
        capTail = (capTail + 1) % kCapSlots;
        capSent += 1;
    }
}

void setPark(int idx) {
    parkedIdx = idx;
    if (parkedIdx >= 0) monitorReady = advanceChannel();
}

void handleCommand(char* line) {
    // Commands: "cap 0|1", "snap N", "park <ch>|0", "band 5g|2.4g|both", "info"
    char* sp = strchr(line, ' ');
    const char* arg = "";
    if (sp) { *sp = 0; arg = sp + 1; }
    if (!strcmp(line, "cap")) {
        const bool on = atoi(arg) != 0;
        if (on && !captureEnabled) { capDropped = 0; capSent = 0; capHead = 0; capTail = 0; }
        captureEnabled = on;
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"cap\",\"cap\":%d}\n", captureEnabled ? 1 : 0);
    } else if (!strcmp(line, "snap")) {
        int n = atoi(arg);
        if (n < 32) n = 32;
        if (n > kCapMaxLen) n = kCapMaxLen;
        capSnapLen = n;
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"snap\",\"snap\":%d}\n", n);
    } else if (!strcmp(line, "park")) {
        const int ch = atoi(arg);
        const int idx = (ch > 0) ? indexOfChannel(ch) : -1;
        setPark((idx >= 0 && chanEnabled(idx)) ? idx : -1);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"park\",\"park\":%d}\n", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
    } else if (!strcmp(line, "band")) {
        if (!strcmp(arg, "5g")) setBandMode(BAND_5G);
        else if (!strcmp(arg, "2.4g") || !strcmp(arg, "24g")) setBandMode(BAND_24G);
        else if (!strcmp(arg, "both")) setBandMode(BAND_BOTH);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"band\",\"band\":\"%s\"}\n", kBandName[bandMode]);
        sendHello();
    } else if (!strcmp(line, "info")) {
        sendHello();
        sendSweep();
    } else {
        Serial.printf("{\"t\":\"err\",\"msg\":\"unknown command\"}\n");
    }
}

void pollSerial() {
    static char line[48];
    static size_t len = 0;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (len) { line[len] = 0; handleCommand(line); }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
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
    const uint32_t now = millis();
    if (!monitorReady) {
        // No channel could be selected yet: keep retrying at the dwell cadence.
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
    if (monitorReady && parkedIdx < 0 && currentIdx < lastIdx) {  // wrapped around: one full sweep done
        sweepCount += 1;
        sendSweep();
    }
}

// ---------------------------------------------------------------------------------------------
// UI (172 x 320 portrait) – three pages, BOOT button cycles them
// ---------------------------------------------------------------------------------------------
lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, const lv_font_t* font = nullptr) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

lv_obj_t* make_panel(lv_obj_t* parent, int height, uint16_t bg, int pad) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), height);
    lv_obj_set_style_bg_color(p, c565(bg), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_pad_all(p, pad, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* make_page(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, c565(BG_565), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* make_header(lv_obj_t* page, const char* title, lv_obj_t** rightLabel) {
    lv_obj_t* header = make_panel(page, 26, PANEL_565, 4);
    lv_obj_t* t = make_label(header, title, c565(WHITE_565), &lv_font_montserrat_14);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 2, 0);
    *rightLabel = make_label(header, "5G", c565(CYAN_565), &lv_font_montserrat_14);
    lv_obj_align(*rightLabel, LV_ALIGN_RIGHT_MID, -2, 0);
    return header;
}

void buildOverviewPage(lv_obj_t* page) {
    make_header(page, "Bandwatch", &chanLabel);

    // Global activity: big number, sweep counter, bar
    lv_obj_t* globalWrap = make_panel(page, 56, PANEL_565, 6);
    globalLabel = make_label(globalWrap, "0", c565(WHITE_565), &lv_font_montserrat_20);
    lv_obj_align(globalLabel, LV_ALIGN_TOP_LEFT, 0, -2);
    lv_obj_t* gl = make_label(globalWrap, "max busy", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(gl, LV_ALIGN_TOP_LEFT, 40, 3);
    sweepLabel = make_label(globalWrap, "sweep 0", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(sweepLabel, LV_ALIGN_TOP_RIGHT, 0, 3);
    globalBar = lv_bar_create(globalWrap);
    lv_bar_set_range(globalBar, 0, 100);
    lv_obj_set_size(globalBar, LV_PCT(100), 14);
    lv_obj_align(globalBar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(globalBar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(globalBar, LV_OPA_40, 0);

    // Top 3 busiest channels: "ch36 78" | mini bar | "412/s"
    lv_obj_t* topBox = make_panel(page, 92, BG_565, 2);
    lv_obj_set_style_pad_row(topBox, 3, 0);
    lv_obj_set_flex_flow(topBox, LV_FLEX_FLOW_COLUMN);
    make_label(topBox, "Top 3", c565(YELLOW_565), &lv_font_montserrat_12);
    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = lv_obj_create(topBox);
        lv_obj_set_size(row, LV_PCT(100), 20);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 5, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        topRows[i] = make_label(row, "--", c565(WHITE_565), &lv_font_montserrat_14);
        lv_obj_set_width(topRows[i], 62);

        lv_obj_t* mini = lv_bar_create(row);
        lv_bar_set_range(mini, 0, 100);
        lv_obj_set_size(mini, 50, 10);
        lv_obj_set_style_bg_color(mini, c565(BLACK_565), 0);
        lv_obj_set_style_bg_opa(mini, LV_OPA_30, 0);
        lv_obj_set_style_radius(mini, 3, 0);
        topBars[i] = mini;

        topRates[i] = make_label(row, "", c565(GREY_565), &lv_font_montserrat_12);
    }

    // Spectrum strip: one bar per channel, grouped by band segment
    lv_obj_t* spec = make_panel(page, 58, PANEL_565, 4);
    lv_obj_t* barsRow = lv_obj_create(spec);
    lv_obj_set_size(barsRow, LV_PCT(100), 36);
    lv_obj_align(barsRow, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(barsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(barsRow, 0, 0);
    lv_obj_set_style_pad_all(barsRow, 0, 0);
    lv_obj_set_style_pad_column(barsRow, 1, 0);
    lv_obj_set_flex_flow(barsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barsRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(barsRow, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kChannelCount; i++) {
        lv_obj_t* b = lv_obj_create(barsRow);
        lv_obj_set_size(b, 5, 2);
        lv_obj_set_style_bg_color(b, c565(DIM_565), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        for (int g = 1; g < 4; g++) if (i == kGroupStart[g]) lv_obj_set_style_margin_left(b, 3, 0);
        specBars[i] = b;
    }
    specLabel = make_label(spec, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(specLabel, LV_PCT(100));
    lv_obj_set_style_text_align(specLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(specLabel, LV_ALIGN_BOTTOM_MID, 0, 2);

    // Last dwell of the current channel: rates, talkers, strong frames
    lv_obj_t* stats = make_panel(page, 40, BG_565, 2);
    statsLine1 = make_label(stats, "", c565(WHITE_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine1, LV_ALIGN_TOP_LEFT, 0, 0);
    statsLine2 = make_label(stats, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine2, LV_ALIGN_TOP_LEFT, 0, 18);

    // Footer: APs, capture state
    footLabel = make_label(page, "APs --", c565(YELLOW_565), &lv_font_montserrat_14);
    lv_obj_set_width(footLabel, LV_PCT(100));
    lv_obj_set_style_text_align(footLabel, LV_TEXT_ALIGN_CENTER, 0);
}

void buildChannelsPage(lv_obj_t* page) {
    make_header(page, "Channels", &listChanLabel);

    // Three columns of 13 rows: "36 [bar] 78" (2.4 GHz, 5 GHz or both, laid out on refresh)
    lv_obj_t* grid = make_panel(page, 262, BG_565, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 2, 0);
    lv_obj_t* cols[3];
    for (int c = 0; c < 3; c++) {
        cols[c] = lv_obj_create(grid);
        lv_obj_set_size(cols[c], 53, LV_PCT(100));
        lv_obj_set_style_bg_opa(cols[c], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cols[c], 0, 0);
        lv_obj_set_style_pad_all(cols[c], 0, 0);
        lv_obj_set_style_pad_row(cols[c], 0, 0);
        lv_obj_set_flex_flow(cols[c], LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(cols[c], LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int i = 0; i < kListSlots; i++) {
        lv_obj_t* row = lv_obj_create(cols[i / 13]);
        listRow[i] = row;
        lv_obj_set_size(row, LV_PCT(100), 20);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        listName[i] = make_label(row, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(listName[i], 20);
        lv_obj_set_style_text_align(listName[i], LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t* bar = lv_bar_create(row);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_size(bar, 14, 8);
        lv_obj_set_style_bg_color(bar, c565(BLACK_565), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        listBar[i] = bar;

        listVal[i] = make_label(row, "-", c565(GREY_565), &lv_font_montserrat_12);
        lv_obj_set_width(listVal[i], 15);
    }

    listFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(listFoot, LV_PCT(100));
    lv_obj_set_style_text_align(listFoot, LV_TEXT_ALIGN_CENTER, 0);
}

void buildSystemPage(lv_obj_t* page) {
    lv_obj_t* hdrRight;
    make_header(page, "System", &hdrRight);
    lv_label_set_text(hdrRight, "v" "1.0");
    lv_obj_t* box = make_panel(page, 280, BG_565, 2);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kSysLines; i++) {
        sysLines[i] = make_label(box, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(sysLines[i], LV_PCT(100));
        lv_label_set_long_mode(sysLines[i], LV_LABEL_LONG_CLIP);
    }
}

void showPage(int n) {
    currentPage = ((n % kPageCount) + kPageCount) % kPageCount;
    for (int i = 0; i < kPageCount; i++) {
        if (i == currentPage) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void buildUi() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, c565(BG_565), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    pages[0] = make_page(scr); buildOverviewPage(pages[0]);
    pages[1] = make_page(scr); buildChannelsPage(pages[1]);
    pages[2] = make_page(scr); buildSystemPage(pages[2]);
    showPage(0);
}

void chanHeaderText(char* buf, size_t n) {
    const char* band = (currentIdx >= 0 && is5g(currentIdx)) ? "5G" : "2.4G";
    if (!monitorReady)       snprintf(buf, n, "no ch");
    else if (parkedIdx >= 0) snprintf(buf, n, "park %u", kChannels[currentIdx]);
    else                     snprintf(buf, n, "%s ch%u", band, kChannels[currentIdx]);
}

void refreshOverview(float global) {
    char buf[64];
    char r1[16], r2[16];
    lv_bar_set_value(globalBar, static_cast<int>(global + 0.5f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(globalBar, scoreColor(global), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%.0f", global);
    lv_label_set_text(globalLabel, buf);
    snprintf(buf, sizeof(buf), "sweep %lu", static_cast<unsigned long>(sweepCount));
    lv_label_set_text(sweepLabel, buf);
    chanHeaderText(buf, sizeof(buf));
    lv_label_set_text(chanLabel, buf);

    int top[3];
    sortTop3(top);
    for (int i = 0; i < 3; i++) {
        if (top[i] < 0) {
            lv_label_set_text(topRows[i], "--");
            lv_label_set_text(topRates[i], "");
            lv_bar_set_value(topBars[i], 0, LV_ANIM_OFF);
            continue;
        }
        const ChannelState& ch = channels[top[i]];
        snprintf(buf, sizeof(buf), "ch%u %.0f", kChannels[top[i]], ch.busyEma);
        lv_label_set_text(topRows[i], buf);
        lv_bar_set_value(topBars[i], static_cast<int>(ch.busyEma + 0.5f), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(topBars[i], scoreColor(ch.busyEma), LV_PART_INDICATOR);
        fmtRate(r1, sizeof(r1), ch.metrics.frames * 1000.0f / kDwellMs, "/s");
        lv_label_set_text(topRates[i], r1);
    }

    const int nEnabled = enabledCount();
    int barW = (150 / (nEnabled > 0 ? nEnabled : 1)) - 1;
    if (barW < 2) barW = 2;
    if (barW > 10) barW = 10;
    for (int i = 0; i < kChannelCount; i++) {
        const ChannelState& ch = channels[i];
        if (!chanEnabled(i)) { lv_obj_add_flag(specBars[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(specBars[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(specBars[i], barW);
        int h = 2;
        lv_color_t col = c565(DIM_565);
        if (ch.unavailable) {
            col = c565(BLACK_565);
        } else if (ch.hasData) {
            h = 2 + static_cast<int>(ch.busyEma * 34.0f / 100.0f);
            col = (ch.busyEma < 3.0f) ? c565(GREY_565) : scoreColor(ch.busyEma);
        }
        if (i == currentIdx && monitorReady) col = c565(CYAN_565);
        lv_obj_set_height(specBars[i], h);
        lv_obj_set_style_bg_color(specBars[i], col, 0);
    }
    lv_label_set_text(specLabel, bandMode == BAND_5G ? "36-64   100-144   149-165"
                                 : bandMode == BAND_24G ? "2.4 GHz channels 1-13"
                                 : "1-13 | 36-64 | 100-144 | 149-165");

    const ChannelState& cur = channels[currentIdx < 0 ? 0 : currentIdx];
    if (cur.hasData) {
        fmtRate(r1, sizeof(r1), cur.metrics.frames * 1000.0f / kDwellMs, " pkt/s");
        fmtRate(r2, sizeof(r2), cur.metrics.bytes * 1000.0f / kDwellMs, " B/s");
        snprintf(buf, sizeof(buf), "ch%u: %s  %s", kChannels[currentIdx], r1, r2);
        lv_label_set_text(statsLine1, buf);
        snprintf(buf, sizeof(buf), "talkers %u  strong %u/%lu  raw %.0f", cur.metrics.unique, cur.metrics.strong,
                 static_cast<unsigned long>(cur.metrics.frames), cur.busyCurrent);
        lv_label_set_text(statsLine2, buf);
    } else {
        lv_label_set_text(statsLine1, "listening...");
        lv_label_set_text(statsLine2, "");
    }

    if (captureEnabled) {
        snprintf(buf, sizeof(buf), "APs %u  " LV_SYMBOL_DOWNLOAD " %lu  drop %lu", lastApSeen,
                 static_cast<unsigned long>(capSent), static_cast<unsigned long>(capDropped));
    } else {
        snprintf(buf, sizeof(buf), "APs %u", lastApSeen);
    }
    lv_label_set_text(footLabel, buf);
}

void refreshChannels(float global) {
    char buf[48];
    chanHeaderText(buf, sizeof(buf));
    lv_label_set_text(listChanLabel, buf);
    int slot = 0;
    for (int i = 0; i < kChannelCount && slot < kListSlots; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        lv_obj_remove_flag(listRow[slot], LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "%u", kChannels[i]);
        lv_label_set_text(listName[slot], buf);
        lv_obj_set_style_text_color(listName[slot], (i == currentIdx && monitorReady) ? c565(CYAN_565)
                                                    : ch.unavailable ? c565(GREY_565) : c565(WHITE_565), 0);
        if (ch.unavailable) {
            lv_label_set_text(listVal[slot], "x");
            lv_bar_set_value(listBar[slot], 0, LV_ANIM_OFF);
        } else if (!ch.hasData) {
            lv_label_set_text(listVal[slot], "-");
            lv_bar_set_value(listBar[slot], 0, LV_ANIM_OFF);
        } else {
            snprintf(buf, sizeof(buf), "%.0f", ch.busyEma);
            lv_label_set_text(listVal[slot], buf);
            lv_bar_set_value(listBar[slot], static_cast<int>(ch.busyEma + 0.5f), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(listBar[slot], scoreColor(ch.busyEma), LV_PART_INDICATOR);
        }
        slot++;
    }
    for (; slot < kListSlots; slot++) lv_obj_add_flag(listRow[slot], LV_OBJ_FLAG_HIDDEN);
    snprintf(buf, sizeof(buf), "max %.0f  sweep %lu  APs %u", global, static_cast<unsigned long>(sweepCount), lastApSeen);
    lv_label_set_text(listFoot, buf);
}

void refreshSystem(float global) {
    char buf[64];
    char r1[16], r2[16];
    int top[3];
    sortTop3(top);
    int n = 0;
    if (top[0] >= 0) {
        const ChannelState& ch = channels[top[0]];
        fmtRate(r1, sizeof(r1), ch.metrics.frames * 1000.0f / kDwellMs, " pkt/s");
        fmtRate(r2, sizeof(r2), ch.metrics.bytes * 1000.0f / kDwellMs, " B/s");
        snprintf(buf, sizeof(buf), "busiest ch%u  score %.0f", kChannels[top[0]], ch.busyEma);
        lv_label_set_text(sysLines[n++], buf);
        snprintf(buf, sizeof(buf), "  %s  %s", r1, r2);
        lv_label_set_text(sysLines[n++], buf);
        snprintf(buf, sizeof(buf), "  talkers %u  strong %u/%lu", ch.metrics.unique, ch.metrics.strong,
                 static_cast<unsigned long>(ch.metrics.frames));
        lv_label_set_text(sysLines[n++], buf);
    } else {
        lv_label_set_text(sysLines[n++], "busiest: listening...");
        lv_label_set_text(sysLines[n++], "");
        lv_label_set_text(sysLines[n++], "");
    }
    const uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "uptime %02lu:%02lu:%02lu   max %.0f", static_cast<unsigned long>(up / 3600),
             static_cast<unsigned long>((up / 60) % 60), static_cast<unsigned long>(up % 60), global);
    lv_label_set_text(sysLines[n++], buf);
    int skipped = 0;
    for (int i = 0; i < kChannelCount; i++) if (chanEnabled(i) && channels[i].unavailable) skipped++;
    snprintf(buf, sizeof(buf), "sweeps %lu  dwell %u ms", static_cast<unsigned long>(sweepCount), static_cast<unsigned>(kDwellMs));
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "band %s  channels %d  skip %d", kBandName[bandMode], enabledCount(), skipped);
    lv_label_set_text(sysLines[n++], buf);
    if (parkedIdx >= 0) snprintf(buf, sizeof(buf), "park: ch%u", kChannels[parkedIdx]);
    else snprintf(buf, sizeof(buf), "park: off (hopping)");
    lv_label_set_text(sysLines[n++], buf);
    if (captureEnabled) snprintf(buf, sizeof(buf), "capture: on  %lu sent  %lu drop", static_cast<unsigned long>(capSent),
                                 static_cast<unsigned long>(capDropped));
    else snprintf(buf, sizeof(buf), "capture: off (host: cap 1)");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "radio %s/%s/%s", errBand == ESP_OK ? "band ok" : "band ERR",
             errCountry == ESP_OK ? "cc ok" : "cc ERR", errPromisc == ESP_OK ? "promisc ok" : "promisc ERR");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "heap %u kB free", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    lv_label_set_text(sysLines[n++], buf);
    lv_label_set_text(sysLines[n++], "");
    lv_label_set_text(sysLines[n++], "BOOT: tap=page  hold=band");
    lv_label_set_text(sysLines[n++], "(5g > 2.4g > both)");
}

void refreshUi() {
    const float global = globalActivityMax();

    // Drive onboard RGB LED based on global activity
    if (global > 75.0f)      setLedColor(LED_RED);
    else if (global > 50.0f) setLedColor(LED_ORANGE);
    else if (global > 25.0f) setLedColor(LED_YELLOW);
    else                     setLedColor(LED_GREEN);

    // AP estimate: peak unique transmitters seen in any dwell within the window
    uint16_t liveUnique;
    portENTER_CRITICAL(&g_accumMux);
    liveUnique = g_accum.unique;
    portEXIT_CRITICAL(&g_accumMux);
    const uint32_t nowMs = millis();
    if (apWindowStartedMs == 0) apWindowStartedMs = nowMs;
    if (liveUnique > apMaxWindow) apMaxWindow = liveUnique; // hold the peak within the window
    if ((nowMs - apWindowStartedMs) >= kApUpdateMs) {
        lastApSeen = apMaxWindow;
        apWindowStartedMs = nowMs;
        apMaxWindow = 0;
    }

    switch (currentPage) {
        case 0: refreshOverview(global); break;
        case 1: refreshChannels(global); break;
        default: refreshSystem(global); break;
    }
}

void uiTimerCb(lv_timer_t* t) {
    (void)t;
    ensureWifiMonitor();
    hopIfNeeded();
    refreshUi();
}

// ---------------------------------------------------------------------------------------------
// BOOT button: tap = next page, hold = cycle band mode (5g -> 2.4g -> both)
// ---------------------------------------------------------------------------------------------
void pollButton() {
    static bool wasDown = false;
    static uint32_t downSince = 0;
    static bool longFired = false;
    static uint32_t lastEdgeMs = 0;
    const uint32_t now = millis();
    const bool down = digitalRead(kBootButtonPin) == LOW;
    if (down != wasDown) {
        if (now - lastEdgeMs < 30) return;   // debounce
        lastEdgeMs = now;
        wasDown = down;
        if (down) {
            downSince = now;
            longFired = false;
        } else if (!longFired) {
            showPage(currentPage + 1);
            refreshUi();
        }
        return;
    }
    if (down && !longFired && (now - downSince) >= kLongPressMs) {
        longFired = true;
        setBandMode(static_cast<BandMode>((bandMode + 1) % 3));
        Serial.printf("{\"t\":\"log\",\"msg\":\"button: band %s\"}\n", kBandName[bandMode]);
        sendHello();
        refreshUi();
    }
}

} // namespace

void Bandwatch_Init(void) {
    pinMode(kBootButtonPin, INPUT_PULLUP);

    // Quick LED self-test: red -> green -> blue (confirms wiring/colour order).
    setLedColor({255, 0, 0}, 100);
    delay(120);
    setLedColor({0, 255, 0}, 100);
    delay(120);
    setLedColor({0, 0, 255}, 100);
    delay(120);
    rgbLedWrite(kRgbPin, 0, 0, 0);

    buildUi();
    lv_timer_create(uiTimerCb, kUiIntervalMs, nullptr);
    ensureWifiMonitor();
    sendHello();
    refreshUi();
}

void Bandwatch_Loop(void) {
    pollSerial();
    pollButton();
    drainCapture();
}
