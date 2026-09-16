#include "bandwatch.h"
#include "devices.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <math.h>
#include <string.h>
#include <sdkconfig.h>
#include <esp_system.h>

#if !CONFIG_SOC_WIFI_SUPPORT_5G
#error "Bandwatch needs a 5 GHz capable target (ESP32-C5). Select 'ESP32C5 Dev Module'."
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------
constexpr const char* kVersion = "1.1";
constexpr uint32_t kDwellMs = 220;          // Dwell per channel (200–400 ms)
constexpr uint32_t kUiIntervalMs = 120;     // UI refresh cadence
constexpr int kStrongThresholdDbm = -65;    // "Strong" frame threshold
constexpr float kBusyEmaAlpha = 0.22f;      // Smoothing within required 0.15–0.30
constexpr int kUniqueSlots = 24;            // Best-effort unique transmitter slots
constexpr int kRgbPin = 8;                  // Onboard WS2812B data pin (Waveshare ESP32-C5-LCD-1.47)
constexpr int kBootButtonPin = 28;          // BOOT key = GPIO28 strap; free to use as an input after boot
constexpr uint32_t kLongPressMs = 700;      // Hold BOOT this long to cycle band mode (or stop a hunt)
constexpr uint32_t kApUpdateMs = 3000;      // AP count refresh cadence
constexpr uint32_t kDevListMs = 2000;       // Device table -> host cadence
constexpr uint32_t kDevFreshMs = 60000;     // Devices older than this are not reported
constexpr uint32_t kDevLcdFreshMs = 20000;  // ... nor shown on the LCD
constexpr uint32_t kBleScanSec = 3;         // Restart the BLE scan (and clear its result cache) this often
constexpr uint32_t kBleHeapFloor = 28000;   // ...or sooner, when the cache has eaten the heap down to this
constexpr const char* kCountryCode = "EU";  // Only affects the regulatory table; we never transmit.

// Channels to sweep. The C5 has ONE radio, so bands are time-shared: a "both" sweep simply
// interleaves 2.4 GHz channels 1-13 with the 5 GHz list below (38 dwells, ~8.4 s per sweep).
// 5 GHz: UNII-1 (36–48), UNII-2A (52–64, DFS), UNII-2C (100–144, DFS), UNII-3 (149–165).
// Receiving on DFS channels is passive; the radio never transmits in promiscuous mode.
// Channels the driver refuses (ESP_ERR_INVALID_ARG) are skipped automatically.
enum BandMode : uint8_t { BAND_5G = 0, BAND_24G = 1, BAND_BOTH = 2, BAND_BLE = 3 };
constexpr int kBandModes = 4;
constexpr const char* kBandName[] = {"5g", "2.4g", "both", "ble"};
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
constexpr int kGroupStart[] = {0, 13, 21, 33};

// Allow every 5 GHz channel the driver knows about (bits 1..28, see wifi_5g_channel_bit_t).
constexpr uint32_t kAll5gChannelMask = 0x1FFFFFFEu;

// Frame capture (streamed to the host over USB serial as base64 lines, host writes the pcap).
constexpr int kCapSlotsMax = 20;
constexpr int kCapSlotsMin = 4;
constexpr uint16_t kCapMaxLen = 1600;
constexpr uint32_t kCapHeapReserve = 14000;   // keep this much heap free after allocating the ring

// LCD pages
enum Page : int { PAGE_OVERVIEW = 0, PAGE_CHANNELS, PAGE_DEVICES, PAGE_HUNT, PAGE_SYSTEM, PAGE_COUNT };
constexpr int kDevRows = 12;

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
constexpr uint16_t ORANGE_565  = 0xFD20;

struct RgbColor { uint8_t r; uint8_t g; uint8_t b; };
constexpr RgbColor LED_GREEN  = {0, 180, 40};
constexpr RgbColor LED_YELLOW = {255, 200, 0};
constexpr RgbColor LED_ORANGE = {255, 120, 0};
constexpr RgbColor LED_RED    = {255, 24, 0};
constexpr RgbColor LED_BLUE   = {0, 60, 255};

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
portMUX_TYPE g_devMux = portMUX_INITIALIZER_UNLOCKED;

WifiDev wifiDevs[kWifiDevSlots];
BleDev bleDevs[kBleDevSlots];

// Hunt target (one MAC, tracked from both radios)
uint8_t huntMac[6] = {0};
volatile bool huntActive = false;
volatile int8_t huntRssi = -127;
volatile uint32_t huntLastMs = 0;
volatile uint32_t huntCount = 0;
char huntLabel[33] = "";
bool huntParked = false;

// Single-producer (Wi-Fi task) / single-consumer (loop) ring buffer for captured frames.
// Allocated from the heap only while a capture runs (32 KB), so it costs nothing otherwise.
CapFrame* capRing = nullptr;
int capSlots = 0;
volatile uint8_t capHead = 0;     // next slot the producer writes
volatile uint8_t capTail = 0;     // next slot the consumer reads
volatile bool captureEnabled = false;
volatile uint16_t capSnapLen = kCapMaxLen;
volatile uint32_t capDropped = 0;
uint32_t capSent = 0;
volatile uint8_t currentChannelNum = 0;
BandMode bandMode = BAND_5G;

inline bool wifiMode() { return bandMode != BAND_BLE; }
inline bool chanEnabled(int idx) {
    if (bandMode == BAND_BLE) return false;
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
bool wifiRunning = false;
esp_err_t errCountry = ESP_FAIL, errBand = ESP_FAIL, errProto = ESP_FAIL, errPromisc = ESP_FAIL;

// BLE
BLEScan* bleScan = nullptr;
bool bleInited = false;
volatile bool bleScanDone = false;
uint32_t bleScanCycles = 0;
uint32_t bleStatusMs = 0;

// UI objects
lv_obj_t* pages[PAGE_COUNT] = {nullptr};
int currentPage = 0;
// overview
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
// channels
lv_obj_t* listChanLabel = nullptr;
constexpr int kListSlots = 39;   // 3 columns x 13 rows
lv_obj_t* listRow[kListSlots] = {nullptr};
lv_obj_t* listName[kListSlots] = {nullptr};
lv_obj_t* listBar[kListSlots] = {nullptr};
lv_obj_t* listVal[kListSlots] = {nullptr};
lv_obj_t* listFoot = nullptr;
// devices
lv_obj_t* devHdrRight = nullptr;
lv_obj_t* devRow[kDevRows] = {nullptr};
lv_obj_t* devName[kDevRows] = {nullptr};
lv_obj_t* devRssi[kDevRows] = {nullptr};
lv_obj_t* devBar[kDevRows] = {nullptr};
lv_obj_t* devFoot = nullptr;
// hunt
lv_obj_t* huntHdrRight = nullptr;
lv_obj_t* huntBig = nullptr;
lv_obj_t* huntBar = nullptr;
lv_obj_t* huntMacLbl = nullptr;
lv_obj_t* huntNameLbl = nullptr;
lv_obj_t* huntInfo1 = nullptr;
lv_obj_t* huntInfo2 = nullptr;
lv_obj_t* huntHint = nullptr;
// system
constexpr int kSysLines = 14;
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
    return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
}

lv_color_t scoreColor(float s) {
    if (s > 70.0f) return c565(RED_565);
    if (s > 40.0f) return c565(YELLOW_565);
    return c565(GREEN_565);
}

lv_color_t rssiColor(int rssi) {
    if (rssi >= -50) return c565(RED_565);
    if (rssi >= -65) return c565(ORANGE_565);
    if (rssi >= -80) return c565(YELLOW_565);
    return c565(CYAN_565);
}

void fmtRate(char* out, size_t n, float perSec, const char* unit) {
    if (perSec >= 10000.0f) snprintf(out, n, "%.0fk%s", perSec / 1000.0f, unit);
    else if (perSec >= 1000.0f) snprintf(out, n, "%.1fk%s", perSec / 1000.0f, unit);
    else snprintf(out, n, "%.0f%s", perSec, unit);
}

void fmtMac(char* out, size_t n, const uint8_t* m) {
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool parseMac(const char* s, uint8_t* out) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

// Write a JSON string literal (quoted, escaped) to Serial.
void printJsonStr(const char* s) {
    Serial.write('"');
    for (; *s; s++) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { Serial.write('\\'); Serial.write(c); }
        else if (c < 0x20) { Serial.printf("\\u%04x", c); }
        else Serial.write(c);
    }
    Serial.write('"');
}

// Non-blocking serial policy: the USB CDC TX buffer is large (see setup) and writes never block. To avoid
// half-written lines when the host is slow or absent, every line checks for room first and is dropped whole.
inline bool serialRoom(size_t n) { return static_cast<size_t>(Serial.availableForWrite()) >= n; }

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
                if (len > 0 && len <= 32) { memcpy(d.ssid, v, len); d.ssid[len] = 0; }
                break;
            case 7:   // Country
                if (len >= 2) { d.cc[0] = v[0]; d.cc[1] = v[1]; d.cc[2] = 0; }
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
                const uint16_t pc = v[p] | (v[p + 1] << 8); p += 2 + 4 * pc;
                if (p + 2 > len) break;
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

void IRAM_ATTR trackWifiDevice(const uint8_t* mac, int8_t rssi, uint8_t fc0, const uint8_t* payload, uint16_t sigLen) {
    const uint32_t now = millis();
    const bool isBeacon = (fc0 == 0x80) || (fc0 == 0x50);   // beacon / probe response
    portENTER_CRITICAL_ISR(&g_devMux);
    const int i = devFindSlot(wifiDevs, kWifiDevSlots, mac);
    WifiDev& d = wifiDevs[i];
    if (d.lastMs == 0 || !macEq(d.mac, mac)) {
        memset(&d, 0, sizeof(d));
        memcpy(d.mac, mac, 6);
        d.maxRssi = rssi;
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
    if (huntActive && macEq(huntMac, mac)) {
        huntRssi = rssi;
        huntLastMs = now;
        huntCount = huntCount + 1;
    }
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
    }

    if (captureEnabled && capRing) {
        const uint8_t head = capHead;
        const uint8_t next = (head + 1) % capSlots;
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
    for (int i = 0; i < kUniqueSlots; i++) g_accum.macHashes[i] = 0;
    portEXIT_CRITICAL(&g_accumMux);
}

// ---------------------------------------------------------------------------------------------
// BLE scanning (runs in the NimBLE host task)
// ---------------------------------------------------------------------------------------------
class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        uint8_t mac[6];
        if (!parseMac(dev.getAddress().toString().c_str(), mac)) return;
        const int8_t rssi = static_cast<int8_t>(dev.getRSSI());
        const uint32_t now = millis();
        char name[21] = "";
        uint16_t company = 0;
        uint8_t appleType = 0;
        if (dev.haveName()) {
            String n = dev.getName();
            strncpy(name, n.c_str(), sizeof(name) - 1);
        }
        if (dev.haveManufacturerData()) {
            String md = dev.getManufacturerData();
            if (md.length() >= 2) company = static_cast<uint8_t>(md[0]) | (static_cast<uint8_t>(md[1]) << 8);
            if (company == 0x004C && md.length() >= 3) appleType = static_cast<uint8_t>(md[2]);
        }
        const uint8_t addrType = dev.getAddressType();
        const uint16_t appearance = dev.haveAppearance() ? dev.getAppearance() : 0;
        const int8_t txPower = dev.haveTXPower() ? dev.getTXPower() : 127;
        uint16_t svc = 0, svcData = 0;
        if (dev.haveServiceUUID() && dev.getServiceUUIDCount() > 0) {
            BLEUUID u = dev.getServiceUUID(0);
            if (u.bitSize() == 16) svc = u.getNative()->u16.value;
        }
        if (dev.haveServiceData() && dev.getServiceDataUUIDCount() > 0) {
            BLEUUID u = dev.getServiceDataUUID(0);
            if (u.bitSize() == 16) svcData = u.getNative()->u16.value;
        }
        const uint8_t bflags = (dev.isConnectable() ? 1 : 0) | (dev.isLegacyAdvertisement() ? 2 : 0) | (dev.isScannable() ? 4 : 0);

        portENTER_CRITICAL(&g_devMux);
        const int i = devFindSlot(bleDevs, kBleDevSlots, mac);
        BleDev& d = bleDevs[i];
        if (d.lastMs == 0 || !macEq(d.mac, mac)) {
            memset(&d, 0, sizeof(d));
            memcpy(d.mac, mac, 6);
            d.maxRssi = rssi;
            d.txPower = 127;
        }
        d.rssi = rssi;
        if (rssi > d.maxRssi) d.maxRssi = rssi;
        if (d.adv < 65535) d.adv++;
        d.lastMs = now;
        d.addrType = addrType;
        if (name[0]) strncpy(d.name, name, sizeof(d.name));
        if (company) d.company = company;
        if (appleType) d.appleType = appleType;
        if (appearance) d.appearance = appearance;
        if (txPower != 127) d.txPower = txPower;
        if (svc) d.svc = svc;
        if (svcData) d.svcData = svcData;
        d.flags |= bflags;
        if (huntActive && macEq(huntMac, mac)) {
            huntRssi = rssi;
            huntLastMs = now;
            huntCount = huntCount + 1;
        }
        portEXIT_CRITICAL(&g_devMux);
    }
};

void bleScanComplete(BLEScanResults) { bleScanDone = true; }

void startBle() {
    if (!bleInited) {
        BLEDevice::init("bandwatch");
        bleScan = BLEDevice::getScan();
        bleScan->setAdvertisedDeviceCallbacks(new AdvCallbacks(), true, true);
        bleScan->setActiveScan(true);
        bleScan->setInterval(100);
        bleScan->setWindow(80);
        bleInited = true;
    }
    bleScan->clearResults();
    bleScanDone = false;
    bleScan->start(kBleScanSec, bleScanComplete, false);
}

void stopBle() {
    if (!bleInited) return;
    bleScan->stop();
    bleScan->clearResults();
    BLEDevice::deinit(false);   // keep controller memory so BLE can be re-initialised later
    bleInited = false;
    bleScan = nullptr;
}

// Called from loop: keep the scan running in cycles so the library's result cache cannot grow unbounded.
void serviceBle() {
    if (bandMode != BAND_BLE || !bleInited) return;
    // The library keeps every device seen during a scan cycle in a heap-allocated result list; in a
    // crowded place that can exhaust the C5's RAM, so cycles are short and cut early when heap runs low.
    if (!bleScanDone && ESP.getFreeHeap() < kBleHeapFloor) {
        bleScan->stop();
        bleScanDone = true;
    }
    if (bleScanDone) {
        bleScanCycles++;
        bleScan->clearResults();
        bleScanDone = false;
        bleScan->start(kBleScanSec, bleScanComplete, false);
    }
}

// ---------------------------------------------------------------------------------------------
// Channel control / Wi-Fi lifecycle
// ---------------------------------------------------------------------------------------------
bool applyChannelIdx(int idx) {
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
    if (!wifiMode() || !wifiRunning) return false;
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

int indexOfChannel(int ch) {
    for (int i = 0; i < kChannelCount; i++) if (kChannels[i] == ch) return i;
    return -1;
}

wifi_band_mode_t toDriverBand(BandMode m) {
    return m == BAND_5G ? WIFI_BAND_MODE_5G_ONLY : m == BAND_24G ? WIFI_BAND_MODE_2G_ONLY : WIFI_BAND_MODE_AUTO;
}

void applyProtocols() {
    wifi_protocols_t protos = {};
    protos.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
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
    bandMode = m;
    if (m == BAND_BLE) {
        if (prev != BAND_BLE) { stopWifi(); startBle(); }
        return;
    }
    if (prev == BAND_BLE) {
        stopBle();
        startWifi();
        return;
    }
    if (parkedIdx >= 0 && !chanEnabled(parkedIdx)) parkedIdx = -1;
    errBand = esp_wifi_set_band_mode(toDriverBand(m));
    delay(100);
    applyProtocols();
    resetChannelStats();
    currentIdx = -1;
    monitorReady = advanceChannel();
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------
float computeBusyScore(const ChannelMetrics& m) {
    const float dwellSec = static_cast<float>(kDwellMs) / 1000.0f;
    const float pps = m.frames / dwellSec;
    const float bps = m.bytes / dwellSec;
    const float strongRatio = (m.frames > 0) ? (static_cast<float>(m.strong) / static_cast<float>(m.frames)) : 0.0f;
    const float ppsScore = clamp01(log1pf(pps) / logf(600.0f));
    const float bpsScore = clamp01(log1pf(bps) / logf(50000.0f));
    const float uniqueScore = clamp01(log1pf(static_cast<float>(m.unique)) / logf(20.0f));
    const float raw = 0.40f * ppsScore + 0.30f * bpsScore + 0.20f * strongRatio + 0.10f * uniqueScore;
    return clamp01(raw) * 100.0f;
}

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
// used from the loop task only (UI timer and host output both run there).
union DevSnap { WifiDev w[kWifiDevSlots]; BleDev b[kBleDevSlots]; };
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
template <typename T>
void sortByRssi(T* a, int n) {   // insertion sort, n <= 96
    for (int i = 1; i < n; i++) {
        T v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].rssi < v.rssi) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

// ---------------------------------------------------------------------------------------------
// Host protocol (USB serial, one JSON object per line; frames as "P ..." base64 lines)
// ---------------------------------------------------------------------------------------------
void printHunt() {
    if (!huntActive) { Serial.print("\"h\":null"); return; }
    const uint32_t last = huntLastMs;
    Serial.printf("\"h\":[%d,%lu,%lu]", huntRssi, static_cast<unsigned long>(last ? millis() - last : 0xFFFFFFFFul),
                  static_cast<unsigned long>(huntCount));
}

void sendHello() {
    if (!serialRoom(700)) return;
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
    char mac[18];
    fmtMac(mac, sizeof(mac), huntMac);
    static const char* const kRst[] = {"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
                                       "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"};
    const int rr = static_cast<int>(esp_reset_reason());
    Serial.printf("],\"park\":%d,\"cap\":%d,\"snap\":%u,\"heap\":%u,\"up\":%lu,\"rst\":\"%s\",\"hunt\":%s%s%s,",
                  parkedIdx >= 0 ? kChannels[parkedIdx] : 0, captureEnabled ? 1 : 0,
                  static_cast<unsigned>(capSnapLen), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(millis() / 1000), (rr >= 0 && rr < 16) ? kRst[rr] : "?",
                  huntActive ? "\"" : "null", huntActive ? mac : "", huntActive ? "\"" : "");
    printHunt();
    Serial.print("}\n");
}

void sendDwell(int idx) {
    if (!serialRoom(260)) return;
    const ChannelState& ch = channels[idx];
    Serial.printf("{\"t\":\"d\",\"c\":%u,\"s\":%.1f,\"r\":%.1f,\"f\":%lu,\"b\":%lu,\"st\":%u,\"u\":%u,"
                  "\"g\":%.1f,\"n\":%lu,\"park\":%d,\"cap\":%d,\"drop\":%lu,",
                  kChannels[idx], ch.busyEma, ch.busyCurrent,
                  static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                  ch.metrics.strong, ch.metrics.unique, globalActivityMax(),
                  static_cast<unsigned long>(sweepCount), parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped));
    printHunt();
    Serial.print("}\n");
}

void sendSweep() {
    if (!serialRoom(1500)) return;
    Serial.printf("{\"t\":\"s\",\"n\":%lu,\"g\":%.1f,\"band\":\"%s\",\"ch\":[", static_cast<unsigned long>(sweepCount),
                  globalActivityMax(), kBandName[bandMode]);
    bool first = true;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        Serial.printf("%s[%u,%.1f,%lu,%lu,%u,%u,%d]", first ? "" : ",", kChannels[i], ch.busyEma,
                      static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                      ch.metrics.strong, ch.metrics.unique, ch.unavailable ? 2 : (ch.hasData ? 0 : 1));
        first = false;
    }
    Serial.printf("],\"aps\":%u,\"drop\":%lu,\"heap\":%u}\n", lastApSeen, static_cast<unsigned long>(capDropped),
                  static_cast<unsigned>(ESP.getFreeHeap()));
}

// BLE-mode heartbeat (no dwells there)
void sendBleStatus() {
    if (!serialRoom(200)) return;
    BleDev tmp[1];
    int n = 0;
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kBleDevSlots; i++) if (bleDevs[i].lastMs && now - bleDevs[i].lastMs <= kDevFreshMs) n++;
    portEXIT_CRITICAL(&g_devMux);
    (void)tmp;
    Serial.printf("{\"t\":\"ble\",\"devs\":%d,\"cycles\":%lu,\"heap\":%u,", n, static_cast<unsigned long>(bleScanCycles),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    printHunt();
    Serial.print("}\n");
}

// Device tables -> host. Wi-Fi: [mac, rssi, max, frames, age_ms, ch, flags, ssid]; BLE: [mac, rssi, max, adv, age_ms, addrType, company, name]
void sendDevices() {
    const uint32_t now = millis();
    char mac[18];
    if (wifiMode()) {
        WifiDev* snap = devSnap.w;
        const int n = snapshotWifi(snap, kWifiDevSlots, kDevFreshMs);
        if (!serialRoom(40 + n * 110)) return;
        Serial.print("{\"t\":\"w\",\"dev\":[");
        for (int i = 0; i < n; i++) {
            fmtMac(mac, sizeof(mac), snap[i].mac);
            const WifiDev& d = snap[i];
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", i ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                          static_cast<unsigned long>(now - d.lastMs), d.ch, d.flags);
            printJsonStr(d.ssid);
            Serial.printf(",%u,%u,%u,%u,%u,%u,\"%s\"]", d.sec, d.pmf, d.phy, d.bw, d.util, d.stations, d.cc[0] ? d.cc : "");
        }
        Serial.print("]}\n");
    } else {
        BleDev* snap = devSnap.b;
        const int n = snapshotBle(snap, kBleDevSlots, kDevFreshMs);
        if (!serialRoom(40 + n * 95)) return;
        Serial.print("{\"t\":\"b\",\"dev\":[");
        for (int i = 0; i < n; i++) {
            fmtMac(mac, sizeof(mac), snap[i].mac);
            const BleDev& d = snap[i];
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", i ? "," : "", mac, d.rssi, d.maxRssi, d.adv,
                          static_cast<unsigned long>(now - d.lastMs), d.addrType, d.company);
            printJsonStr(d.name);
            Serial.printf(",%u,%d,%u,%u,%u,%u]", d.appearance, d.txPower, d.svc, d.svcData, d.appleType, d.flags);
        }
        Serial.print("]}\n");
    }
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void writeBase64(const uint8_t* d, size_t n) {
    char out[68];
    size_t i = 0;
    while (i < n) {
        size_t o = 0;
        const size_t chunkEnd = (i + 48 < n) ? i + 48 : n;
        while (i + 2 < chunkEnd) {
            const uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
            out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
            out[o++] = kB64[(v >> 6) & 63];  out[o++] = kB64[v & 63];
            i += 3;
        }
        if (i < chunkEnd && chunkEnd == n) {
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

void drainCapture() {
    int budget = 8;
    while (capRing && capTail != capHead && budget-- > 0) {
        const CapFrame& f = capRing[capTail];
        if (!serialRoom((f.capLen * 4) / 3 + 40)) return;
        Serial.printf("P %u %d %lu %u ", f.channel, f.rssi, static_cast<unsigned long>(f.ts_us), f.len);
        writeBase64(f.data, f.capLen);
        Serial.write('\n');
        capTail = (capTail + 1) % capSlots;
        capSent += 1;
    }
}

void setPark(int idx) {
    parkedIdx = idx;
    if (parkedIdx >= 0 && wifiMode()) monitorReady = advanceChannel();
}

void showPage(int n);

void lookupHuntLabel() {
    huntLabel[0] = 0;
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kWifiDevSlots; i++)
        if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, huntMac) && wifiDevs[i].ssid[0]) { strncpy(huntLabel, wifiDevs[i].ssid, 32); break; }
    if (!huntLabel[0])
        for (int i = 0; i < kBleDevSlots; i++)
            if (bleDevs[i].lastMs && macEq(bleDevs[i].mac, huntMac) && bleDevs[i].name[0]) { strncpy(huntLabel, bleDevs[i].name, 32); break; }
    portEXIT_CRITICAL(&g_devMux);
}

void startHunt(const uint8_t* mac, int ch) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(huntMac, mac, 6);
    huntRssi = -127;
    huntLastMs = 0;
    huntCount = 0;
    huntActive = true;
    portEXIT_CRITICAL(&g_devMux);
    lookupHuntLabel();
    huntParked = false;
    if (ch > 0 && wifiMode()) {
        const int idx = indexOfChannel(ch);
        if (idx >= 0 && chanEnabled(idx)) { setPark(idx); huntParked = true; }
    }
    showPage(PAGE_HUNT);
}

void stopHunt() {
    huntActive = false;
    if (huntParked) { setPark(-1); huntParked = false; }
    if (currentPage == PAGE_HUNT) showPage(wifiMode() ? PAGE_OVERVIEW : PAGE_DEVICES);
}

void handleCommand(char* line) {
    // Commands: "cap 0|1", "snap N", "park <ch>|0", "band 5g|2.4g|both|ble", "hunt <mac> [ch]" | "hunt 0", "info"
    char* sp = strchr(line, ' ');
    char* arg = const_cast<char*>("");
    if (sp) { *sp = 0; arg = sp + 1; }
    if (!strcmp(line, "cap")) {
        const bool on = atoi(arg) != 0 && wifiMode();
        if (on && !captureEnabled) {
            capDropped = 0; capSent = 0; capHead = 0; capTail = 0;
            if (!capRing) {
                const uint32_t freeHeap = ESP.getMaxAllocHeap();
                int slots = (freeHeap > kCapHeapReserve) ? static_cast<int>((freeHeap - kCapHeapReserve) / sizeof(CapFrame)) : 0;
                if (slots > kCapSlotsMax) slots = kCapSlotsMax;
                if (slots >= kCapSlotsMin) {
                    capRing = static_cast<CapFrame*>(malloc(sizeof(CapFrame) * slots));
                    capSlots = capRing ? slots : 0;
                }
                if (!capRing && serialRoom(120))
                    Serial.printf("{\"t\":\"err\",\"msg\":\"capture buffer: not enough free heap (%u)\"}\n", static_cast<unsigned>(freeHeap));
                else if (serialRoom(100))
                    Serial.printf("{\"t\":\"log\",\"msg\":\"capture ring: %d slots\"}\n", capSlots);
            }
        }
        captureEnabled = on && capRing;
        if (!captureEnabled && capRing) { CapFrame* r = capRing; capRing = nullptr; capSlots = 0; free(r); }
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
        else if (!strcmp(arg, "ble")) setBandMode(BAND_BLE);
        if (!wifiMode() && (currentPage == PAGE_OVERVIEW || currentPage == PAGE_CHANNELS)) showPage(PAGE_DEVICES);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"band\",\"band\":\"%s\"}\n", kBandName[bandMode]);
        sendHello();
    } else if (!strcmp(line, "hunt")) {
        uint8_t mac[6];
        char* sp2 = strchr(arg, ' ');
        int ch = 0;
        if (sp2) { *sp2 = 0; ch = atoi(sp2 + 1); }
        if (parseMac(arg, mac)) startHunt(mac, ch);
        else stopHunt();
        char m[18];
        fmtMac(m, sizeof(m), huntMac);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":%s%s%s,\"park\":%d}\n", huntActive ? "\"" : "null",
                      huntActive ? m : "", huntActive ? "\"" : "", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
    } else if (!strcmp(line, "info")) {
        sendHello();
        if (wifiMode()) sendSweep();
        sendDevices();
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
    if (!wifiMode() || !wifiRunning) return;
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

// ---------------------------------------------------------------------------------------------
// UI (172 x 320 portrait) – pages, BOOT button cycles the ones that apply
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
    *rightLabel = make_label(header, "", c565(CYAN_565), &lv_font_montserrat_14);
    lv_obj_align(*rightLabel, LV_ALIGN_RIGHT_MID, -2, 0);
    return header;
}

lv_obj_t* make_row(lv_obj_t* parent, int height, int colGap) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, colGap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* make_bar(lv_obj_t* parent, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    return bar;
}

void buildOverviewPage(lv_obj_t* page) {
    make_header(page, "Bandwatch", &chanLabel);

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

    lv_obj_t* topBox = make_panel(page, 92, BG_565, 2);
    lv_obj_set_style_pad_row(topBox, 3, 0);
    lv_obj_set_flex_flow(topBox, LV_FLEX_FLOW_COLUMN);
    make_label(topBox, "Top 3", c565(YELLOW_565), &lv_font_montserrat_12);
    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = make_row(topBox, 20, 5);
        topRows[i] = make_label(row, "--", c565(WHITE_565), &lv_font_montserrat_14);
        lv_obj_set_width(topRows[i], 62);
        topBars[i] = make_bar(row, 50, 10);
        topRates[i] = make_label(row, "", c565(GREY_565), &lv_font_montserrat_12);
    }

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

    lv_obj_t* stats = make_panel(page, 40, BG_565, 2);
    statsLine1 = make_label(stats, "", c565(WHITE_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine1, LV_ALIGN_TOP_LEFT, 0, 0);
    statsLine2 = make_label(stats, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine2, LV_ALIGN_TOP_LEFT, 0, 18);

    footLabel = make_label(page, "APs --", c565(YELLOW_565), &lv_font_montserrat_14);
    lv_obj_set_width(footLabel, LV_PCT(100));
    lv_obj_set_style_text_align(footLabel, LV_TEXT_ALIGN_CENTER, 0);
}

void buildChannelsPage(lv_obj_t* page) {
    make_header(page, "Channels", &listChanLabel);
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
        lv_obj_t* row = make_row(cols[i / 13], 20, 2);
        listRow[i] = row;
        listName[i] = make_label(row, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(listName[i], 20);
        lv_obj_set_style_text_align(listName[i], LV_TEXT_ALIGN_RIGHT, 0);
        listBar[i] = make_bar(row, 14, 8);
        listVal[i] = make_label(row, "-", c565(GREY_565), &lv_font_montserrat_12);
        lv_obj_set_width(listVal[i], 15);
    }
    listFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(listFoot, LV_PCT(100));
    lv_obj_set_style_text_align(listFoot, LV_TEXT_ALIGN_CENTER, 0);
}

void buildDevicesPage(lv_obj_t* page) {
    make_header(page, "Devices", &devHdrRight);
    lv_obj_t* box = make_panel(page, 246, BG_565, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kDevRows; i++) {
        lv_obj_t* row = make_row(box, 20, 3);
        devRow[i] = row;
        devName[i] = make_label(row, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(devName[i], 84);
        lv_label_set_long_mode(devName[i], LV_LABEL_LONG_CLIP);
        devRssi[i] = make_label(row, "", c565(GREY_565), &lv_font_montserrat_12);
        lv_obj_set_width(devRssi[i], 26);
        lv_obj_set_style_text_align(devRssi[i], LV_TEXT_ALIGN_RIGHT, 0);
        devBar[i] = make_bar(row, 44, 8);
    }
    devFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(devFoot, LV_PCT(100));
    lv_obj_set_style_text_align(devFoot, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(devFoot, LV_LABEL_LONG_CLIP);
}

void buildHuntPage(lv_obj_t* page) {
    make_header(page, "Hunt", &huntHdrRight);
    lv_obj_t* box = make_panel(page, 120, PANEL_565, 6);
    huntBig = make_label(box, "--", c565(WHITE_565), &lv_font_montserrat_28);
    lv_obj_align(huntBig, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t* unit = make_label(box, "dBm", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 40);
    huntBar = lv_bar_create(box);
    lv_bar_set_range(huntBar, 0, 100);
    lv_obj_set_size(huntBar, LV_PCT(100), 22);
    lv_obj_align(huntBar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(huntBar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(huntBar, LV_OPA_40, 0);
    lv_obj_t* lab = make_label(box, "far", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    lab = make_label(box, "near", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_RIGHT, 0, -24);

    lv_obj_t* info = make_panel(page, 120, BG_565, 2);
    lv_obj_set_style_pad_row(info, 4, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    huntMacLbl = make_label(info, "", c565(CYAN_565), &lv_font_montserrat_14);
    huntNameLbl = make_label(info, "", c565(WHITE_565), &lv_font_montserrat_14);
    lv_obj_set_width(huntNameLbl, LV_PCT(100));
    lv_label_set_long_mode(huntNameLbl, LV_LABEL_LONG_CLIP);
    huntInfo1 = make_label(info, "", c565(GREY_565), &lv_font_montserrat_12);
    huntInfo2 = make_label(info, "", c565(GREY_565), &lv_font_montserrat_12);
    huntHint = make_label(page, "hold BOOT to stop hunting", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(huntHint, LV_PCT(100));
    lv_obj_set_style_text_align(huntHint, LV_TEXT_ALIGN_CENTER, 0);
}

void buildSystemPage(lv_obj_t* page) {
    lv_obj_t* hdrRight;
    make_header(page, "System", &hdrRight);
    char v[8];
    snprintf(v, sizeof(v), "v%s", kVersion);
    lv_label_set_text(hdrRight, v);
    lv_obj_t* box = make_panel(page, 280, BG_565, 2);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kSysLines; i++) {
        sysLines[i] = make_label(box, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(sysLines[i], LV_PCT(100));
        lv_label_set_long_mode(sysLines[i], LV_LABEL_LONG_CLIP);
    }
}

bool pageAvailable(int n) {
    if (n == PAGE_OVERVIEW || n == PAGE_CHANNELS) return wifiMode();
    if (n == PAGE_HUNT) return huntActive;
    return true;
}

// Only the visible page exists as LVGL objects (the others are rebuilt on demand): the C5 has no PSRAM and
// the Wi-Fi/BLE stacks need most of the 320 KB, so ~300 permanent widgets are not affordable.
void showPage(int n) {
    n = ((n % PAGE_COUNT) + PAGE_COUNT) % PAGE_COUNT;
    for (int tries = 0; tries < PAGE_COUNT && !pageAvailable(n); tries++) n = (n + 1) % PAGE_COUNT;
    if (pages[currentPage] && n == currentPage) return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pages[i]) { lv_obj_delete(pages[i]); pages[i] = nullptr; }
    }
    currentPage = n;
    lv_obj_t* pg = make_page(lv_scr_act());
    pages[n] = pg;
    switch (n) {
        case PAGE_OVERVIEW: buildOverviewPage(pg); break;
        case PAGE_CHANNELS: buildChannelsPage(pg); break;
        case PAGE_DEVICES:  buildDevicesPage(pg); break;
        case PAGE_HUNT:     buildHuntPage(pg); break;
        default:            buildSystemPage(pg); break;
    }
}

void buildUi() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, c565(BG_565), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    showPage(PAGE_OVERVIEW);
}

void chanHeaderText(char* buf, size_t n) {
    if (!wifiMode()) { snprintf(buf, n, "BLE"); return; }
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

inline int rssiPct(int rssi) {   // -100 dBm -> 0, -30 dBm -> 100
    int p = (rssi + 100) * 100 / 70;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

struct DevRowInfo { uint8_t mac[6]; int8_t rssi; bool ap; char label[33]; };
DevRowInfo devRows[kDevRows];
int devRowCount = 0;

void refreshDevices() {
    static uint32_t lastSortMs = 0;
    const uint32_t now = millis();
    if (now - lastSortMs >= 500) {
        lastSortMs = now;
        if (wifiMode()) {
            const int wn = snapshotWifi(devSnap.w, kWifiDevSlots, kDevLcdFreshMs);
            sortByRssi(devSnap.w, wn);
            devRowCount = wn < kDevRows ? wn : kDevRows;
            for (int i = 0; i < devRowCount; i++) {
                memcpy(devRows[i].mac, devSnap.w[i].mac, 6);
                devRows[i].rssi = devSnap.w[i].rssi;
                devRows[i].ap = devSnap.w[i].flags & 1;
                strncpy(devRows[i].label, devSnap.w[i].ssid, 32); devRows[i].label[32] = 0;
            }
        } else {
            const int bn = snapshotBle(devSnap.b, kBleDevSlots, kDevLcdFreshMs);
            sortByRssi(devSnap.b, bn);
            devRowCount = bn < kDevRows ? bn : kDevRows;
            for (int i = 0; i < devRowCount; i++) {
                memcpy(devRows[i].mac, devSnap.b[i].mac, 6);
                devRows[i].rssi = devSnap.b[i].rssi;
                devRows[i].ap = false;
                strncpy(devRows[i].label, devSnap.b[i].name, 32); devRows[i].label[32] = 0;
            }
        }
    }
    char buf[48];
    const int n = devRowCount;
    snprintf(buf, sizeof(buf), "%s %d", wifiMode() ? "WiFi" : "BLE", n);
    lv_label_set_text(devHdrRight, buf);
    for (int i = 0; i < kDevRows; i++) {
        if (i >= n) { lv_obj_add_flag(devRow[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(devRow[i], LV_OBJ_FLAG_HIDDEN);
        const uint8_t* mac = devRows[i].mac;
        const int rssi = devRows[i].rssi;
        const char* label = devRows[i].label;
        const bool ap = devRows[i].ap;
        if (label[0]) snprintf(buf, sizeof(buf), "%s%s", ap ? "* " : "", label);
        else snprintf(buf, sizeof(buf), "%s%02x:%02x:%02x", ap ? "* " : "", mac[3], mac[4], mac[5]);
        lv_label_set_text(devName[i], buf);
        lv_obj_set_style_text_color(devName[i], (huntActive && macEq(mac, huntMac)) ? c565(CYAN_565) : c565(WHITE_565), 0);
        snprintf(buf, sizeof(buf), "%d", rssi);
        lv_label_set_text(devRssi[i], buf);
        lv_bar_set_value(devBar[i], rssiPct(rssi), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(devBar[i], rssiColor(rssi), LV_PART_INDICATOR);
    }
    if (wifiMode()) snprintf(buf, sizeof(buf), "* = AP (beacons)  seen < 20 s");
    else snprintf(buf, sizeof(buf), "BLE scan cycle %lu  seen < 20 s", static_cast<unsigned long>(bleScanCycles));
    lv_label_set_text(devFoot, buf);
}

void refreshHunt() {
    char buf[64];
    const uint32_t last = huntLastMs;
    const int rssi = huntRssi;
    const bool seen = last != 0;
    const uint32_t age = seen ? millis() - last : 0;
    lv_label_set_text(huntHdrRight, wifiMode() ? (parkedIdx >= 0 ? "parked" : "hopping") : "BLE");
    if (seen && age < 5000) snprintf(buf, sizeof(buf), "%d", rssi);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(huntBig, buf);
    lv_bar_set_value(huntBar, (seen && age < 5000) ? rssiPct(rssi) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(huntBar, rssiColor(rssi), LV_PART_INDICATOR);
    fmtMac(buf, sizeof(buf), huntMac);
    lv_label_set_text(huntMacLbl, buf);
    if (!huntLabel[0]) lookupHuntLabel();
    lv_label_set_text(huntNameLbl, huntLabel[0] ? huntLabel : "(no name seen)");
    if (seen) snprintf(buf, sizeof(buf), "seen %.1f s ago  hits %lu", age / 1000.0f, static_cast<unsigned long>(huntCount));
    else snprintf(buf, sizeof(buf), "not seen yet");
    lv_label_set_text(huntInfo1, buf);
    if (wifiMode()) snprintf(buf, sizeof(buf), "listening ch %u%s", currentChannelNum, parkedIdx >= 0 ? " (parked)" : " (hopping)");
    else snprintf(buf, sizeof(buf), "BLE scan, %lu cycles", static_cast<unsigned long>(bleScanCycles));
    lv_label_set_text(huntInfo2, buf);
}

void refreshSystem(float global) {
    char buf[64];
    char r1[16], r2[16];
    int n = 0;
    if (wifiMode()) {
        int top[3];
        sortTop3(top);
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
    } else {
        int devs = 0;
        const uint32_t now = millis();
        portENTER_CRITICAL(&g_devMux);
        for (int i = 0; i < kBleDevSlots; i++) if (bleDevs[i].lastMs && now - bleDevs[i].lastMs <= kDevFreshMs) devs++;
        portEXIT_CRITICAL(&g_devMux);
        snprintf(buf, sizeof(buf), "BLE mode: %d devices (60 s)", devs);
        lv_label_set_text(sysLines[n++], buf);
        snprintf(buf, sizeof(buf), "  scan cycles %lu", static_cast<unsigned long>(bleScanCycles));
        lv_label_set_text(sysLines[n++], buf);
        lv_label_set_text(sysLines[n++], "  Wi-Fi sniffing paused");
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
    if (huntActive) { char m[18]; fmtMac(m, sizeof(m), huntMac); snprintf(buf, sizeof(buf), "hunt: %s", m); }
    else snprintf(buf, sizeof(buf), "hunt: off");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "radio %s/%s/%s", errBand == ESP_OK ? "band ok" : "band ERR",
             errCountry == ESP_OK ? "cc ok" : "cc ERR", errPromisc == ESP_OK ? "promisc ok" : "promisc ERR");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "heap %u kB free", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    lv_label_set_text(sysLines[n++], buf);
    lv_label_set_text(sysLines[n++], "BOOT: tap=page  hold=band");
    lv_label_set_text(sysLines[n++], "(5g > 2.4g > both > ble)");
    for (; n < kSysLines; n++) lv_label_set_text(sysLines[n], "");
}

void driveLed(float global) {
    if (huntActive) {
        const uint32_t last = huntLastMs;
        if (!last || millis() - last > 5000) { setLedColor(LED_BLUE, 15); return; }
        const int r = huntRssi;
        if (r >= -50)      setLedColor(LED_RED, 100);
        else if (r >= -62) setLedColor(LED_ORANGE, 80);
        else if (r >= -75) setLedColor(LED_YELLOW, 60);
        else if (r >= -88) setLedColor(LED_GREEN, 40);
        else               setLedColor(LED_BLUE, 30);
        return;
    }
    if (!wifiMode()) { setLedColor(LED_BLUE, 25); return; }
    if (global > 75.0f)      setLedColor(LED_RED);
    else if (global > 50.0f) setLedColor(LED_ORANGE);
    else if (global > 25.0f) setLedColor(LED_YELLOW);
    else                     setLedColor(LED_GREEN);
}

void refreshUi() {
    const float global = wifiMode() ? globalActivityMax() : 0.0f;
    driveLed(global);

    if (wifiMode()) {
        uint16_t liveUnique;
        portENTER_CRITICAL(&g_accumMux);
        liveUnique = g_accum.unique;
        portEXIT_CRITICAL(&g_accumMux);
        const uint32_t nowMs = millis();
        if (apWindowStartedMs == 0) apWindowStartedMs = nowMs;
        if (liveUnique > apMaxWindow) apMaxWindow = liveUnique;
        if ((nowMs - apWindowStartedMs) >= kApUpdateMs) {
            lastApSeen = apMaxWindow;
            apWindowStartedMs = nowMs;
            apMaxWindow = 0;
        }
    }

    if (!pageAvailable(currentPage) || !pages[currentPage]) showPage(currentPage + 1);
    switch (currentPage) {
        case PAGE_OVERVIEW: refreshOverview(global); break;
        case PAGE_CHANNELS: refreshChannels(global); break;
        case PAGE_DEVICES:  refreshDevices(); break;
        case PAGE_HUNT:     refreshHunt(); break;
        default:            refreshSystem(global); break;
    }
}

void uiTimerCb(lv_timer_t* t) {
    (void)t;
    hopIfNeeded();
    refreshUi();
}

// ---------------------------------------------------------------------------------------------
// BOOT button: tap = next page, hold = cycle band mode (or stop the hunt when on the hunt page)
// ---------------------------------------------------------------------------------------------
void pollButton() {
    static bool wasDown = false;
    static uint32_t downSince = 0;
    static bool longFired = false;
    static uint32_t lastEdgeMs = 0;
    const uint32_t now = millis();
    const bool down = digitalRead(kBootButtonPin) == LOW;
    if (down != wasDown) {
        if (now - lastEdgeMs < 30) return;
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
        if (currentPage == PAGE_HUNT && huntActive) {
            stopHunt();
            if (serialRoom(80)) Serial.print("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":null,\"park\":0}\n");
        } else {
            setBandMode(static_cast<BandMode>((bandMode + 1) % kBandModes));
            if (serialRoom(120)) Serial.printf("{\"t\":\"log\",\"msg\":\"button: band %s\"}\n", kBandName[bandMode]);
            sendHello();
        }
        refreshUi();
    }
}

} // namespace

void Bandwatch_Init(void) {
    pinMode(kBootButtonPin, INPUT_PULLUP);
    memset(wifiDevs, 0, sizeof(wifiDevs));
    memset(bleDevs, 0, sizeof(bleDevs));

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
    static uint32_t lastDevMs = 0;
    pollSerial();
    pollButton();
    drainCapture();
    serviceBle();
    const uint32_t now = millis();
    if (now - lastDevMs >= kDevListMs) {
        lastDevMs = now;
        sendDevices();
    }
    if (!wifiMode() && now - bleStatusMs >= 1000) {
        bleStatusMs = now;
        sendBleStatus();
    }
}
