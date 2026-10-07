#pragma once
// Bandwatch shared core: tunables, the mode/channel model, and state that more than one module touches.
// The radios (wifi_sniff / ble_scan / ieee154), capture sinks (capture / sd_sink), deauth diagnostics
// (deauth_diag), host protocol (host_proto) and LCD UI (lcd_ui) are separate files; this is what they share.
// Everything here that lands in DRAM is small on purpose — see docs/DEVELOPER.md §16 for the RAM budget.
#include <stdint.h>
#include <Arduino.h>
#include <esp_err.h>
#include <esp_wifi_types.h>
#include <FS.h>
#include "devices.h"

// LVGL timer type, for the uiTimerCb signature (full definition comes in with lvgl where it is used).
struct _lv_timer_t;
typedef struct _lv_timer_t lv_timer_t;

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------
constexpr const char* kVersion = "1.18";
constexpr uint32_t kDwellMs = 220;          // Dwell per channel (200–400 ms)
constexpr uint32_t kUiIntervalMs = 120;     // UI refresh cadence
constexpr int kStrongThresholdDbm = -65;    // "Strong" frame threshold
constexpr float kBusyEmaAlpha = 0.22f;      // Smoothing within required 0.15–0.30
constexpr int kUniqueSlots = 24;            // Best-effort unique transmitter slots
constexpr int kRgbPin = 8;                  // Onboard WS2812B data pin (Waveshare ESP32-C5-LCD-1.47)
constexpr int kBootButtonPin = 28;          // BOOT key = GPIO28 strap; free to use as an input after boot
constexpr uint32_t kLongPressMs = 700;      // Hold BOOT this long to cycle band mode (or stop a hunt)
constexpr uint32_t kSplashShowMs = 900;     // Mode splash duration for host- or boot-driven band changes
constexpr uint32_t kSplashStepMs = 700;     // BOOT held: cadence of stepping through the splashes (== kLongPressMs, so it's even from button-down)
constexpr uint32_t kSplashTailMs = 700;     // After release, linger on the chosen mode's splash
constexpr uint32_t kBootLogoMs = 2000;      // Boot picture (bootlogo/) shown before the boot mode card
constexpr uint32_t kWrapLogoMs = 5000;      // How long a picture stays up when stopped on the walk's photo slot (and its tap-step)
constexpr uint32_t kApUpdateMs = 3000;      // AP count refresh cadence
constexpr uint32_t kDevListMs = 2000;       // Device table -> host cadence
constexpr uint32_t kDevFreshMs = 60000;     // Devices older than this are not reported
constexpr uint32_t kDevLcdFreshMs = 20000;  // ... nor shown on the LCD
constexpr uint32_t kBleActiveWindowMs = 4000;   // how long to scan actively after a new scannable device
constexpr uint32_t kBleSwitchMinMs = 2000;      // never flip the scan mode more often than this
constexpr const char* kCountryCode = "EU";  // Only affects the regulatory table; we never transmit.
constexpr uint32_t kDeauthMaxMs = 5UL * 60UL * 1000UL;  // Dead-man's switch: auto-stop a deauth attack after this long
                                                          // even if the host/serial link drops mid-attack.
constexpr uint32_t kSdBudgetUs = 8000;       // max time per loop spent writing SD, so channel hopping keeps time

// Spectrum mode (BAND_SPEC): the 802.15.4 radio's energy-detect primitive measures raw RF energy per
// channel with no packet decode, across the 2.4 GHz 15.4 channels 11-26 (~2402-2480 MHz, 5 MHz bins). It
// is the only true noise-floor/energy reading this board exposes; Wi-Fi/5 GHz have no such API (RSSI is
// only ever attached to a decoded frame). See docs/DEVELOPER.md.
constexpr uint32_t kEdDurationSym = 128;     // energy-detect window per sample, in 16 us symbols (~2 ms) -
                                             // long enough that a window usually overlaps a Wi-Fi burst/beacon,
                                             // so bursty traffic shows up (a 128 us window misses ~99% of it)
constexpr uint32_t kEdDwellMs = 60;          // spec dwell: much shorter than kDwellMs (~25 samples is plenty),
                                             // so a full 16-channel sweep is ~1 s instead of ~3.5 s
constexpr int kEdFloorDbm = -95;             // bottom of the on-screen/scored energy range
constexpr int kEdCeilDbm  = -20;             // top of that range
// Fine spectrum: spec mode sweeps 2400-2483 MHz in specStepMhz steps, tuning off the 15.4 channel grid via
// ieee802154_ll_set_freq() (DEVELOPER §18). step 5 ~= the old 16-channel view; 2 (default) and 1 are finer.
constexpr int kSpecLoMhz = 2400;
constexpr int kSpecHiMhz = 2483;
constexpr int kSpecMaxBins = kSpecHiMhz - kSpecLoMhz + 1;   // 84 (1 MHz step, the finest)
constexpr uint8_t kSpecStepDefault = 2;

// Channels to sweep. The C5 has ONE radio, so bands are time-shared: a "both" sweep simply
// interleaves 2.4 GHz channels 1-13 with the 5 GHz list below (38 dwells, ~8.4 s per sweep).
// 5 GHz: UNII-1 (36–48), UNII-2A (52–64, DFS), UNII-2C (100–144, DFS), UNII-3 (149–165).
// Receiving on DFS channels is passive; the radio never transmits in promiscuous mode.
// Channels the driver refuses (ESP_ERR_INVALID_ARG) are skipped automatically.
// Modes: three Wi-Fi sweeps, Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing on
// channels 11-26, and a 2.4 GHz energy-detect spectrum sweep (BAND_SPEC, reuses the 15.4 radio and its
// 11-26 channel set but measures raw energy instead of decoding). All share the single 2.4/5 GHz radio,
// so only one runs at once.
enum BandMode : uint8_t { BAND_5G = 0, BAND_24G = 1, BAND_BOTH = 2, BAND_BLE = 3, BAND_154 = 4, BAND_SPEC = 5 };
constexpr int kBandModes = 6;
constexpr const char* kBandName[] = {"5g", "2.4g", "both", "ble", "154", "spec"};
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

// 802.15.4 channel number (11-26) -> center frequency in MHz (channel 11 = 2405 MHz, 5 MHz spacing).
inline int ch154Freq(int ch) { return 2405 + 5 * (ch - 11); }
// Map a raw energy reading (dBm) onto the shared 0-100 bar/score range used by the LCD and dashboard.
inline float edDbmToScore(int dbm) {
    if (dbm <= kEdFloorDbm) return 0.0f;
    if (dbm >= kEdCeilDbm) return 100.0f;
    return static_cast<float>(dbm - kEdFloorDbm) * 100.0f / static_cast<float>(kEdCeilDbm - kEdFloorDbm);
}

// Advertising-channel access address (BT Core Spec Vol 6 Part B): shared by the BLE pcap frame builder
// and the SD writer's pseudo-header.
constexpr uint32_t kAdvAccessAddr = 0x8E89BED6;

inline void putLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void putLE32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Non-blocking serial policy: the USB CDC TX buffer is large (see setup in bandwatch.ino) and writes never
// block. To avoid half-written lines when the host is slow or absent, every line checks for room first and
// is dropped whole.
inline bool serialRoom(size_t n) { return static_cast<size_t>(Serial.availableForWrite()) >= n; }

// Replace control characters in a string captured off the air (SSID, BLE name, country code) with '.'.
// Bytes >= 0x80 are left alone so UTF-8 names survive. Without this a hostile or corrupt beacon can put
// control bytes in the table, where printJsonStr expands each to a 6-byte \u escape and blows past the
// serialRoom() budget that keeps JSON lines from being truncated mid-write. (Wi-Fi + BLE RX paths.)
inline void IRAM_ATTR sanitizeText(char* s, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) s[i] = '.';
    }
}

// LCD pages (only the visible one exists as LVGL objects — see showPage()).
enum Page : int { PAGE_OVERVIEW = 0, PAGE_CHANNELS, PAGE_SPECTRUM, PAGE_DEVICES, PAGE_HUNT, PAGE_SYSTEM, PAGE_COUNT };

struct RgbColor { uint8_t r; uint8_t g; uint8_t b; };
inline void setLedColor(const RgbColor& c, uint8_t brightness = 60) {
    const uint16_t scale = static_cast<uint16_t>(brightness) * 255 / 100;
    const uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(c.r) * scale) / 255);
    const uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(c.g) * scale) / 255);
    const uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(c.b) * scale) / 255);
    rgbLedWrite(kRgbPin, r, g, b);  // Arduino-ESP32 built-in WS2812 driver (RMT), GRB order
}

// ---------------------------------------------------------------------------------------------
// Shared state types and instances (instances live with their owning module, externed here)
// ---------------------------------------------------------------------------------------------
// Frame capture: streamed to the host over USB serial as base64 lines; the SD sink mirrors the same frames.
constexpr int kCapSlotsMax = 20;
constexpr int kCapSlotsMin = 4;
constexpr uint16_t kCapMaxLen = 1600;

// Per-dwell metrics: accumulated by the RX paths under g_accumMux, consumed at dwell end.
struct Accum {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint16_t strong = 0;
    uint16_t unique = 0;
    uint16_t macHashes[kUniqueSlots] = {0};
    uint8_t macFill = 0;
    // C6 top talker: strongest frame's transmitter this dwell. One instance (g_accum), read by sendDwell
    // before resetAccum() wipes it, so no per-channel storage. bestRssi == -128 means "no attributable frame".
    uint8_t bestMac[6] = {0};
    int8_t  bestRssi = -128;
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
    int8_t edMin = 0;          // Spectrum mode (BAND_SPEC): raw energy over the dwell, in dBm
    int8_t edMax = -128;
    int8_t edMean = 0;
    uint16_t edSamples = 0;    // energy-detect samples folded in this dwell (0 = none)
    bool hasData = false;
    bool unavailable = false;  // Driver rejected esp_wifi_set_channel for this channel
};

// One fine-spectrum bin (spec mode): raw energy over the dwell at one off-grid frequency, in dBm.
struct SpecBin {
    int8_t edMin = 0;
    int8_t edMean = 0;
    int8_t edMax = -128;
    uint16_t edSamples = 0;
};
extern uint8_t specStepMhz;                                 // 1 | 2 | 5 MHz; default kSpecStepDefault
extern SpecBin specFine[kSpecMaxBins];
extern int currentSpecMhz;                                  // frequency the fine sweep is currently parked on
inline int specBinCount() { return (kSpecHiMhz - kSpecLoMhz) / specStepMhz + 1; }
inline int specBinMhz(int i) { return kSpecLoMhz + i * specStepMhz; }

// One captured frame, as it lands in the ring before either sink streams it out.
struct CapFrame {
    uint32_t ts_us;
    uint16_t len;      // original MPDU length (sig_len)
    uint16_t capLen;   // bytes stored
    int8_t rssi;
    uint8_t channel;
    uint8_t data[kCapMaxLen];
};

// Hunt target, tracked from all three radios. mac = 6-byte MAC (Wi-Fi/BLE); key = 802.15.4 key
// (extended address or 0xFF 0xFE pan-short marker form). kind: 0 = MAC, 1 = 802.15.4 key.
struct Hunt {
    uint8_t mac[6] = {};
    uint8_t key[8] = {};
    uint8_t kind = 0;
    volatile bool active = false;
    volatile int8_t rssi = -127;
    volatile uint32_t lastMs = 0;
    volatile uint32_t count = 0;
    char label[33] = "";
    bool parked = false;
};

// Deauth attack: spoof a BSSID and broadcast deauth/disassoc frames until stopped, so every station on that
// network drops (they usually reconnect — with capture running you can grab the EAPOL handshakes).
struct Deauth {
    uint8_t bssid[6] = {};
    // Targeted mode ("dca"): kick one named station instead of every client of the BSS. targetMac is the
    // station, bssid stays the AP we spoof. The flag is explicit rather than inferred from targetMac being
    // non-zero, because a real station MAC may legitimately start 00:00 (the IANA 00:00:5E range, for one)
    // and a first-two-bytes test silently demoted such a target back to a broadcast kick.
    uint8_t targetMac[6] = {};
    volatile bool targeted = false;
    volatile bool active = false;
    bool parked = false;          // like hunt.parked: we hold the park on the target's channel
    volatile uint32_t sent = 0, txFail = 0;
    uint32_t startMs = 0;         // millis() when the current attack started; serviceDeauth enforces kDeauthMaxMs
    bool dumped = false;          // DIAGNOSTIC: dump the built frame once per attack
    // Driver-internal path (deauth_diag): word passed as arg0, holding the STA hmac pointer (g_ic+16). Padded so
    // the driver's seq counter — which send_setup increments at &flag+210 — lands in our own memory instead
    // of a random neighbor.
    uint8_t slotPad[256];
    uint8_t hstate = 0xFF;        // *hmac+312: picks the DA/SA/BSSID mapping inside send_setup (0 / 1 / 3)
};

// BLE. Scan policy: passive only listens; active sends SCAN_REQ and so collects the scan responses that carry
// most device names, at the cost of putting us on the air. "auto" stays passive and opens a short active
// window when a new *scannable* address shows up with no name yet - enough to learn the name, then quiet
// again. The mode is frozen while a capture runs so one pcap is not half passive and half active.
enum BleScanMode : uint8_t { BLE_SCAN_PASSIVE = 0, BLE_SCAN_ACTIVE = 1, BLE_SCAN_AUTO = 2 };
struct BleState {
    bool inited = false;
    volatile bool scanDone = false;
    uint32_t cycles = 0;
    volatile uint32_t advSeen = 0;   // advertising reports received (one per on-air packet)
    BleScanMode mode = BLE_SCAN_AUTO;
    bool active = false;             // what discovery is actually running right now
    volatile uint32_t activeUntilMs = 0;  // auto mode: stay active until this millis()
    uint32_t lastSwitchMs = 0;
    uint32_t switches = 0;
};

// microSD sink (sd_sink). FATFS is mounted only while the card is actually in use: mounting costs ~30 KB
// and BLE mode cuts its scans short below ~28 KB free.
struct SdSink {
    bool mounted = false, capEnabled = false;
    bool readActive = false;      // an sdread is streaming a file out
    bool cardPresent = false;     // seen at boot
    uint32_t cardMb = 0;          // cached so hello can report it without mounting
    File file;
    uint8_t* buf = nullptr;
    size_t bufLen = 0;
    uint32_t frames = 0, bytes = 0, dropped = 0, lastFlushMs = 0, errors = 0;
    bool ioFailed = false;        // a capture write failed (card pulled?): sdServiceFlush closes the capture
    char path[48] = "";
};

// Mode helpers (used by every module).
extern BandMode bandMode;
inline bool wifiMode() { return bandMode <= BAND_BOTH; }
inline bool mode154() { return bandMode == BAND_154; }
inline bool modeSpec() { return bandMode == BAND_SPEC; }   // 2.4 GHz energy-detect spectrum sweep
inline bool hopMode() { return wifiMode() || mode154() || modeSpec(); }   // modes that sweep channels
inline bool chanEnabled(int idx) {
    switch (bandMode) {
        case BAND_5G:   return is5g(idx);
        case BAND_24G:  return kChanBand[idx] == CB_24G;
        case BAND_BOTH: return !is154(idx);
        case BAND_154:  return is154(idx);
        case BAND_SPEC: return is154(idx);   // energy-detect sweeps the 2.4 GHz 15.4 channels 11-26
        default:        return false;
    }
}
inline uint32_t dwellMs() { return modeSpec() ? kEdDwellMs : kDwellMs; }   // spec scans faster
int enabledCount();

// Device table listing (bandwatch.cpp), loop task only (UI timer and host output both run there). Instead of
// copying whole tables into a 4 KB scratch (the old DevSnap union), collect*Refs() copies only a compact
// {rssi, slot, identity} per fresh device under g_devMux and sorts those; the caller then re-fetches each full
// record with fetch*Dev(), which re-validates the slot under a brief lock (saves ~3.3 KB static). A row whose
// slot was evicted/reused between the two steps fails the identity check and is simply skipped that cycle.
struct DevRef { int8_t rssi; uint8_t idx; uint8_t key[8]; };   // one array, sized to the largest table
extern DevRef g_devRefs[kWifiDevSlots];
int collectWifiRefs(DevRef* refs, int maxN, uint32_t freshMs);
int collectBleRefs(DevRef* refs, int maxN, uint32_t freshMs);
int collect154Refs(DevRef* refs, int maxN, uint32_t freshMs);
bool fetchWifiDev(const DevRef& r, WifiDev& out, uint32_t freshMs);
bool fetchBleDev(const DevRef& r, BleDev& out, uint32_t freshMs);
bool fetch154Dev(const DevRef& r, Dev154& out, uint32_t freshMs);
template <typename T>
inline void sortByRssi(T* a, int n) {   // insertion sort, n <= 96
    for (int i = 1; i < n; i++) {
        T v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].rssi < v.rssi) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

// State shared across modules (instances live with their owning module).
extern volatile Accum g_accum;
extern portMUX_TYPE g_accumMux, g_devMux;
extern ChannelState channels[kChannelCount];
extern volatile uint8_t currentChannelNum;
extern int currentIdx;                // index into kChannels (-1 = not placed yet)
extern int parkedIdx;                 // >= 0: stay on this channel instead of hopping
extern uint32_t sweepCount;
extern bool monitorReady;             // the radio is sitting on a usable channel
extern bool wifiRunning;              // Wi-Fi driver up (also true in 15.4 mode? no — one radio, see setBandMode)
extern bool r154Running;              // 802.15.4 driver up (instance in ieee154.cpp)
extern bool specRunning;              // energy-detect spectrum sweep up (15.4 radio, no RX armed; ieee154.cpp)
// A sweep mode is actually running on its radio (gates channel advance / dwell hopping).
inline bool hopActive() { return (wifiMode() && wifiRunning) || (mode154() && r154Running) || (modeSpec() && specRunning); }

// esp_wifi_* setup results, reported by hello / the system page (set while starting Wi-Fi).
extern esp_err_t errCountry, errBand, errProto, errPromisc;

// Capture ring (capture.cpp).
extern CapFrame* capRing;
extern volatile bool captureEnabled;   // USB sink
extern volatile bool capActive;        // either sink wants frames: the RX paths gate on this
extern bool trackAddr1;              // tier-1 receiver-side sightings (set by the "addr1" command)
extern volatile uint16_t capSnapLen;
extern volatile uint32_t capDropped;
extern uint32_t capSent;             // frames streamed to the USB sink since the ring was allocated

// Scoring / dwell (bandwatch.cpp).
float globalActivityMax();
void sortTop3(int outIdx[3]);
int quietestChannel();   // C8: idx of the least-busy channel with data, or -1 if none measured yet
void hopIfNeeded();                  // called from uiTimerCb: finish a dwell and advance when due

// Device tables (bandwatch.cpp): fixed-size open-addressing hashes, updated under g_devMux.
extern WifiDev wifiDevs[kWifiDevSlots];
extern BleDev bleDevs[kBleDevSlots];
extern Dev154 devs154[kDev154Slots];

// Hunt / deauth / BLE scan / SD sink state.
extern Hunt hunt;
extern Deauth deauth;
extern BleState bleScan;
extern SdSink sd;

// Beacon-injection self-test (deauth_diag.cpp), toggled from the host with "txtest".
extern volatile bool txTestActive;
extern uint32_t txTestSent, txTestFail;
extern volatile uint8_t kickFc;      // DIAGNOSTIC: FC byte0 on the internal kick path ("kickfc <hex>")
extern volatile bool useInternalKick;   // "kickpath 1" = driver-internal slot (dead); default 0 = raw TX

void fmtMac(char* out, size_t n, const uint8_t* m);

// Wall clock: the host sends "time <epoch>"; without it timestamps fall back to uptime (1970-based).
extern uint32_t epochBase;            // epoch seconds at millis() == epochBaseMs
extern uint32_t epochBaseMs;
extern bool epochValid;

// Cross-module functions.
void startWifi(); void stopWifi();
wifi_band_mode_t toDriverBand(BandMode m);   // Wi-Fi driver's band enum (setBandMode + startWifi)
void applyProtocols();                       // esp_wifi_set_protocols, result in errProto
void resetChannelStats();                    // clear per-channel history + sweep counter (bandwatch.cpp)
void startBle();  void stopBle();  void serviceBle();
void start154();  void stop154();
// Spectrum (energy-detect) path (ieee154.cpp). start/stop power the 15.4 radio with no RX armed;
// edReset/edKick/edSnapshot manage the per-channel energy accumulator (filled in driver context).
void startSpectrum();  void stopSpectrum();
void edReset();
void edKick();                                              // arm one energy-detect window on the current channel
void edSetFreqMhz(int mhz);                                 // POC: tune synth off the channel grid (2400-2483 MHz)
void serviceSpectrum();                                    // re-arm the next ED window from the loop task
void edSnapshot(int8_t& mn, int8_t& mx, int8_t& mean, uint16_t& n);
bool advanceChannel();
int indexOfChannel(int ch);           // in the current mode's channel set
int indexOfChannel154(int ch);
void setBandMode(BandMode m);
void resetAccum();
void setPark(int idx);                // >= 0: hold this channel instead of hopping; -1: resume

// Capture ring (capture.cpp): reserve a slot, fill it, commit. nullptr when full: drop already counted.
CapFrame* IRAM_ATTR capReserve(uint8_t& nextHead);
void IRAM_ATTR capCommit(uint8_t nextHead);
bool ensureCapRing();
bool refitCapRing();
uint32_t lcdPageHeadroomB();   // lcd_ui.cpp: heap a switch to the heaviest page would take
void releaseCapture();
void syncCapActive();
void drainCapture();
void writeBase64(const uint8_t* d, size_t n);

// Live LCD mirror: when on, the LVGL flush path streams each dirty region as an "M x y w h <base64 RGB565>"
// serial line, and the last flush of every LVGL refresh adds an "MF <seq> <complete>" frame marker; the host
// publishes its framebuffer only at markers, so it never shows a half-updated frame. Default off, not
// persisted (like park and hunt). A region that doesn't fit the TX buffer is folded into one pending repair
// rectangle (a union bounding box) that serviceMirror() re-sends in TX-sized strips; `mirror 1` starts with
// the whole screen as that rectangle. See docs/DEVELOPER.md section 19.
extern bool g_mirror;                                                   // host_proto.cpp owns the command flag
void mirrorOnFlush(int x1, int y1, int x2, int y2, const uint8_t* px, bool last);   // one flushed region
void serviceMirror();        // strip-paced re-send of the repair rectangle, called from the loop (lcd_ui.cpp)
void mirrorRequestFull();    // mark the whole screen for re-send (on enable, or an explicit full frame)
void mirrorNoteDrop(int x1, int y1, int x2, int y2);   // a region didn't fit the TX buffer: add it to the repair
bool mirrorFrameEnd();       // last flush of a refresh: in-flight strips landed; true if a repair is still pending
void stepPage(int dir);      // host page-step (BOOT-tap emulation): dir>0 next page, dir<0 previous (lcd_ui.cpp)

// SD sink (sd_sink.cpp).
bool sdMount();                     // also queried by the "sdinfo" command
void sdUnmount();     // release FATFS (~30 KB) unless a capture or sdread owns the card
void sdSetPresent(bool present, const char* why);   // the one place card presence changes (LCD face on a change)
uint8_t sdProbeR1();         // raw CMD0 reply: 0x01 = a card answered, 0xFF = empty slot
void sdServicePresence();    // loop: probe an idle, unmounted card every 2 s
void showSdFace(bool happy, const char* why);   // lcd_ui.cpp: sad face on removal, happy on insertion
void serviceSdFace();        // lcd_ui.cpp: take the face down after its 3 s
void eventsNudge();          // events.cpp: attach a just-inserted card now
void sdProbeAtBoot();
void sdCloseCapture();
bool sdOpenCapture();
bool sdWriteFrame(const CapFrame& f);   // one frame -> record + link-layer header + payload; false on error
void serviceSdRead();
void sdListFiles();
void sdReadFile(const char* path);
void sdServiceFlush();                  // fsync cadence, called from drainCapture()

// Deauth + diagnostics (deauth_diag.cpp).
void startDeauth(const uint8_t* mac);
void startDeauthTargeted(const uint8_t* clientMac, const uint8_t* apBssid);
void stopDeauth();
void serviceDeauth();

// C1 probe-request mapping: the Wi-Fi task queues each *directed* probe request (a client naming the network it
// wants); serviceProbes() (host_proto.cpp, loop task) dedups (MAC, SSID) pairs and emits {"t":"pr"} lines.
struct ProbeEvt { uint8_t mac[6]; int8_t rssi; uint8_t ch; char ssid[33]; };
constexpr int kProbeQ = 8;
extern ProbeEvt g_probeQ[kProbeQ];
extern volatile uint8_t probeHead, probeTail;   // single producer (Wi-Fi task) / single consumer (loop)
extern volatile uint32_t probeDropped;
void serviceProbes();

// C4 event log (events.cpp): radio paths call eventFlag() under g_devMux when they create a device slot;
// serviceEvents() (loop) classifies, buffers CSV rows and flushes them to /events.csv on the card.
enum : uint8_t { EVF_BLE = 0x01, EVF_RANDOM = 0x02, EVF_TIER1 = 0x04 };
struct EvtPending { uint8_t mac[6]; int8_t rssi; uint8_t ch; uint8_t surv; uint8_t flags; };
constexpr int kEvtQ = 16;
extern EvtPending g_evtQ[kEvtQ];
extern volatile uint8_t evtHead, evtTail;
extern volatile uint32_t evtQDropped;
extern volatile bool g_eventsOn;
struct EventStats { uint32_t written = 0, pending = 0, surv = 0, fresh = 0, dropped = 0, errors = 0, qDropped = 0, baseFile = 0, waiting = 0; bool cardOk = false; };
extern EventStats g_evStats;
void eventFlag(const uint8_t* mac, int8_t rssi, uint8_t ch, uint8_t surv, uint8_t flags);
bool eventsEnable();
void eventsDisable();
void serviceEvents();
void sendEventStatus();   // host_proto.cpp: {"t":"ev"} every 5 s while armed
int eventsBaseCount();
extern bool g_eventsWanted;   // settings.cpp: restored "events" flag, applied in Bandwatch_Init
void loadSurvExtra();   // /surveil.csv extra surveillance OUIs, read at boot while the card is mounted

// Hunt (bandwatch.cpp). Callers hold g_devMux and have already evaluated the match for their radio kind.
void IRAM_ATTR noteHuntHit(bool isTarget, int8_t rssi, uint32_t now);
void huntIdText(char* out, size_t n);   // "aa:bb:.." (MAC) or extended 15.4 key / pan-short form
void lookupHuntLabel();                 // best-effort name/pan for the LCD + serial; empty if nothing seen yet
void startHunt(const uint8_t* mac, int ch);
void startHunt154(const uint8_t* key);
void stopHunt();

// Host protocol (host_proto.cpp).
void sendHello();
void sendSweep();
void sendDwell(int idx);
void sendBleStatus();
void sendDevices();
void pollSerial();
const char* bleScanModeName();

// Settings persistence (settings.cpp). NVS-backed so a walk-around device boots where it was left: band
// mode + the scalar policies (addr1 / blescan / specstep / snap). Park and hunt/deauth are deliberately
// NOT persisted - a reboot must stop transmitting and must not come up silently parked (see settings.cpp).
void loadSettings();    // restore into g_restoredMode + the scalar globals; call before the radios start
void saveSettings();    // write current settings to NVS; call on user-commit, not per walk-step
extern BandMode g_restoredMode;   // mode read from NVS at boot (BAND_5G if none/corrupt); applied by Init
extern bool g_settingsRestored;   // true once a stored blob was loaded (for the boot log line)

// LCD UI (lcd_ui.cpp); currentPage + lastApSeen are read by host_proto.cpp as well.
extern int currentPage;
extern uint16_t lastApSeen;
void buildUi();
void refreshUi();
void showPage(int n);
void showBandSplash(BandMode m, uint32_t durMs);   // flash the mode's name card before its scan page takes over
void pollButton();
void uiTimerCb(lv_timer_t* t);   // declared here so the core file can create the lv timer
