// Event log to microSD (C4): the untethered "what happened on this walk" record. Two kinds of row go to
// /events.csv - a surveillance-OUI match (first per device per session) and a *new* device, meaning a globally
// unique MAC that is not in the /seen.csv baseline on the same card. New MACs are appended to /seen.csv as they
// are logged, so the baseline grows with every walk and a device only counts as new once.
//
// RAM: nothing while off. On, the baseline set (kBaseCap 32-bit hashes = 10 KB) and the pending-row buffer
// (~2 KB) live on the heap. The card is NOT kept mounted (FATFS is ~30 KB, rule 11): rows buffer here and the
// card is mounted just long enough to append them, every kFlushMs or once kFlushRows are waiting. While sdcap
// records, the card belongs to the capture: flushes wait (rows keep buffering; overflow is counted, not hidden).
//
// Card missing / pulled / swapped: "events on" means *armed*. With no card the log still buffers surveillance rows
// and retries the mount every kRetryMs; novelty waits, because with no baseline every device would look new. A
// failed mount or write marks the card lost (rows are kept, SD.end() is forced), and the next successful mount -
// the same card re-inserted or a different one - reloads /seen.csv from that card before anything is written.
//
// Producers are the radio paths (Wi-Fi task / NimBLE host task), which only call eventFlag() under g_devMux
// when they create a device slot; everything else runs on the loop task. docs/DEVELOPER.md §20.
#include "bandwatch_core.h"
#include <SD.h>
#include <algorithm>
#include "surv_ouis.h"

volatile bool g_eventsOn = false;
EvtPending g_evtQ[kEvtQ];
volatile uint8_t evtHead = 0, evtTail = 0;
volatile uint32_t evtQDropped = 0;
EventStats g_evStats;

namespace {
constexpr char kEventsPath[] = "/events.csv";
constexpr char kEventsOld[]  = "/events.old.csv";
constexpr char kSeenPath[]   = "/seen.csv";
constexpr int kBaseLoad = 2048;            // newest entries of /seen.csv kept in RAM (the file itself may be longer)
constexpr int kBaseCap = kBaseLoad + 512;  // room for this session's new devices on top
constexpr int kSurvSeenCap = 32;           // surveillance devices already logged this session
constexpr size_t kRowBuf = 2048;           // pending CSV text
constexpr int kNewMacCap = 48;             // pending /seen.csv appends
constexpr int kFlushRows = 16;
constexpr uint32_t kFlushMs = 60000;
constexpr uint32_t kRetryMs = 30000;       // with no usable card: how often to try mounting again
constexpr uint32_t kRotateBytes = 1024UL * 1024UL;

struct EvState {
    uint32_t* base = nullptr;   // sorted MAC hashes: /seen.csv + this session's new devices
    int baseN = 0;
    uint32_t survSeen[kSurvSeenCap];
    int survN = 0;
    char* rows = nullptr;       // pending CSV rows
    size_t rowsLen = 0;
    int rowsN = 0;
    uint8_t (*newMacs)[6] = nullptr;
    int newN = 0;
    uint32_t lastFlushMs = 0;
    uint32_t lastTryMs = 0;
    bool baseLoaded = false;    // /seen.csv read from the card currently in the slot; novelty is off until then
} ev;

uint32_t macHash32(const uint8_t* m) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ m[i]) * 16777619u;
    return h;
}

int baseFind(uint32_t h, bool& found) {   // lower bound
    int lo = 0, hi = ev.baseN;
    while (lo < hi) { const int mid = (lo + hi) / 2; if (ev.base[mid] < h) lo = mid + 1; else hi = mid; }
    found = lo < ev.baseN && ev.base[lo] == h;
    return lo;
}

bool baseInsert(uint32_t h) {
    bool found;
    const int at = baseFind(h, found);
    if (found) return false;
    if (ev.baseN >= kBaseCap) return true;   // full: still new, just not remembered in RAM (it is still in /seen.csv)
    memmove(ev.base + at + 1, ev.base + at, (ev.baseN - at) * sizeof(uint32_t));
    ev.base[at] = h;
    ev.baseN++;
    return true;
}

bool parseMac(const char* s, uint8_t* out) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

// Read /seen.csv, keeping the newest kBaseLoad entries (a ring over the file), then sort for binary search.
void loadBaseline() {
    ev.baseN = 0;
    ev.baseLoaded = true;   // a card without /seen.csv is a valid, empty baseline
    g_evStats.baseFile = 0;
    File f = SD.open(kSeenPath, FILE_READ);
    if (!f) return;
    char line[40];
    int ring = 0, total = 0;
    while (f.available()) {
        const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        uint8_t mac[6];
        if (!parseMac(line, mac)) continue;
        ev.base[ring] = macHash32(mac);
        ring = (ring + 1) % kBaseLoad;
        total++;
    }
    f.close();
    ev.baseN = total < kBaseLoad ? total : kBaseLoad;
    std::sort(ev.base, ev.base + ev.baseN);
    ev.baseN = static_cast<int>(std::unique(ev.base, ev.base + ev.baseN) - ev.base);
    g_evStats.baseFile = total;
}

void addRow(const char* kind, const EvtPending& e, const char* extra) {
    char row[128], mac[18];
    fmtMac(mac, sizeof(mac), e.mac);
    const uint32_t up = millis();
    char ep[24] = "";
    if (epochValid) {
        const uint32_t d = up - epochBaseMs;
        snprintf(ep, sizeof(ep), "%lu%03lu", static_cast<unsigned long>(epochBase + d / 1000),
                 static_cast<unsigned long>(d % 1000));
    }
    const int n = snprintf(row, sizeof(row), "%s,%lu,%s,%s,%d,%u,%s,%s\n", ep, static_cast<unsigned long>(up), kind, mac,
                           e.rssi, e.ch, (e.flags & EVF_BLE) ? "ble" : "wifi", extra);
    if (n <= 0 || ev.rowsLen + n > kRowBuf) { g_evStats.dropped++; return; }
    memcpy(ev.rows + ev.rowsLen, row, n);
    ev.rowsLen += n;
    ev.rowsN++;
    g_evStats.pending = ev.rowsN;
}

void cardLost() {
    g_evStats.errors++;
    g_evStats.cardOk = false;
    ev.baseLoaded = false;   // whatever comes back may be a different card with a different baseline
    if (!sd.capEnabled && !sd.readActive) { SD.end(); sd.mounted = false; sdSetPresent(false, "event log buffering"); }
}

// Mount the card and, if it is new to us (first time, or back after a failure), load its baseline.
// Returns with the card mounted; the caller unmounts when wasMounted was false.
bool attachCard() {
    if (!sdMount()) { if (g_evStats.cardOk || ev.baseLoaded) cardLost(); else g_evStats.cardOk = false; return false; }
    if (!ev.baseLoaded) {
        loadBaseline();
        // Devices first seen while the card was out are still waiting in newMacs (they go to /seen.csv on this
        // flush); put them back in the RAM set, or the reload would let them count as new a second time.
        for (int i = 0; i < ev.newN; i++) baseInsert(macHash32(ev.newMacs[i]));
    }
    g_evStats.cardOk = true;
    return true;
}

// Mount, append, rotate, unmount. Returns false (rows kept) when the card is busy or missing.
bool flush() {
    if (!ev.rowsN && !ev.newN) return true;
    if (sd.capEnabled || sd.readActive) return false;   // the card belongs to the capture / sdread right now
    const bool wasMounted = sd.mounted;
    if (!attachCard()) return false;
    bool ok = true;
    if (ev.rowsN) {
        File f = SD.open(kEventsPath, FILE_APPEND);
        if (!f) ok = false;
        else {
            if (f.size() == 0) f.print("epoch_ms,up_ms,kind,id,rssi,ch,radio,extra\n");
            ok = f.write(reinterpret_cast<const uint8_t*>(ev.rows), ev.rowsLen) == ev.rowsLen;
            const uint32_t size = f.size();
            f.close();
            if (ok && size > kRotateBytes) {   // boring rotation: keep one previous file
                SD.remove(kEventsOld);
                SD.rename(kEventsPath, kEventsOld);
            }
        }
        if (ok) { g_evStats.written += ev.rowsN; ev.rowsLen = 0; ev.rowsN = 0; }
    }
    if (ok && ev.newN) {
        File f = SD.open(kSeenPath, FILE_APPEND);
        if (!f) ok = false;
        else {
            char mac[18];
            for (int i = 0; i < ev.newN; i++) { fmtMac(mac, sizeof(mac), ev.newMacs[i]); f.print(mac); f.print('\n'); }
            f.close();
            g_evStats.baseFile += ev.newN;
            ev.newN = 0;
        }
    }
    if (!ok) { cardLost(); return false; }   // pulled mid-write: rows stay buffered for the next card
    if (!wasMounted) sdUnmount();
    g_evStats.pending = ev.rowsN;
    ev.lastFlushMs = millis();
    return ok;
}

void freeAll() {
    free(ev.base); ev.base = nullptr;
    free(ev.rows); ev.rows = nullptr;
    free(ev.newMacs); ev.newMacs = nullptr;
    ev.baseN = ev.survN = ev.newN = ev.rowsN = 0;
    ev.rowsLen = 0;
}
} // namespace

// Radio side, called under g_devMux when a device slot is created. Copy and go.
void IRAM_ATTR eventFlag(const uint8_t* mac, int8_t rssi, uint8_t ch, uint8_t surv, uint8_t flags) {
    if (!g_eventsOn) return;
    const bool randomized = (flags & EVF_BLE) ? (flags & EVF_RANDOM) : (mac[0] & 0x02);
    if (!surv && randomized) return;   // rotating privacy addresses are "new" every few minutes: noise, not news
    const uint8_t head = evtHead, next = static_cast<uint8_t>((head + 1) % kEvtQ);
    if (next == evtTail) { evtQDropped = evtQDropped + 1; return; }
    EvtPending& e = g_evtQ[head];
    memcpy(e.mac, mac, 6);
    e.rssi = rssi; e.ch = ch; e.surv = surv; e.flags = flags | (randomized ? EVF_RANDOM : 0);
    evtHead = next;
}

bool eventsEnable() {
    if (g_eventsOn) return true;
    ev.base = static_cast<uint32_t*>(malloc(kBaseCap * sizeof(uint32_t)));
    ev.rows = static_cast<char*>(malloc(kRowBuf));
    ev.newMacs = static_cast<uint8_t(*)[6]>(malloc(kNewMacCap * 6));
    if (!ev.base || !ev.rows || !ev.newMacs) { freeAll(); return false; }
    ev.survN = ev.newN = ev.rowsN = 0;
    ev.rowsLen = 0;
    ev.baseLoaded = false;
    ev.lastFlushMs = ev.lastTryMs = millis();
    if (!sd.capEnabled && !sd.readActive) {   // a capture owns the card: the first flush after it loads the baseline
        const bool wasMounted = sd.mounted;
        if (attachCard() && !wasMounted) sdUnmount();
    }
    g_evStats.pending = 0;
    evtTail = evtHead;   // drop anything flagged before we were ready
    g_eventsOn = true;
    return true;
}

void eventsDisable() {
    if (!g_eventsOn) return;
    g_eventsOn = false;
    flush();   // best effort: a busy card keeps the rows only until we free them, so say so in the counters
    if (ev.rowsN) g_evStats.dropped += ev.rowsN;
    freeAll();
    g_evStats.pending = 0;
}

int eventsBaseCount() { return ev.baseN; }

// A card just appeared: attach on the next serviceEvents() pass instead of waiting out kRetryMs.
void eventsNudge() { ev.lastTryMs = millis() - kRetryMs; }

// Loop side: classify what the radios flagged, buffer rows, flush on size or time.
void serviceEvents() {
    if (!g_eventsOn) return;
    for (;;) {
        EvtPending e;
        portENTER_CRITICAL(&g_devMux);
        const bool have = evtTail != evtHead;
        if (have) { e = g_evtQ[evtTail]; evtTail = static_cast<uint8_t>((evtTail + 1) % kEvtQ); }
        portEXIT_CRITICAL(&g_devMux);
        if (!have) break;
        const uint32_t h = macHash32(e.mac);
        if (e.surv) {
            bool seen = false;
            for (int i = 0; i < ev.survN; i++) if (ev.survSeen[i] == h) { seen = true; break; }
            if (!seen) {
                if (ev.survN < kSurvSeenCap) ev.survSeen[ev.survN++] = h;
                char extra[40];
                snprintf(extra, sizeof(extra), "%s%s", kSurvName[e.surv < kSurvNames ? e.surv : 0],
                         (e.flags & EVF_TIER1) ? " (dest only)" : "");
                addRow("surv", e, extra);
                g_evStats.surv++;
            }
        }
        if (e.flags & EVF_RANDOM) continue;   // surveillance logged above; randomized MACs never count as new
        if (!ev.baseLoaded) { g_evStats.waiting++; continue; }   // no card yet: cannot tell new from known
        if (baseInsert(h)) {
            addRow("new", e, (e.flags & EVF_TIER1) ? "dest only" : "");
            g_evStats.fresh++;
            if (ev.newN < kNewMacCap) memcpy(ev.newMacs[ev.newN++], e.mac, 6);
        }
    }
    g_evStats.qDropped = evtQDropped;
    const uint32_t now = millis();
    // No usable card: try again every kRetryMs (an SD.begin with no card blocks the loop briefly, so not every pass).
    if (!ev.baseLoaded && !sd.capEnabled && !sd.readActive && now - ev.lastTryMs >= kRetryMs) {
        ev.lastTryMs = now;
        const bool wasMounted = sd.mounted;
        if (attachCard()) {
            flush();   // rows that waited for a card go out now
            if (!wasMounted) sdUnmount();
        }
    }
    if (ev.rowsN >= kFlushRows || ev.newN >= kNewMacCap || ((ev.rowsN || ev.newN) && now - ev.lastFlushMs >= kFlushMs)) {
        if (!flush()) ev.lastFlushMs = now;   // busy/missing card: retry on the next interval, not every loop
    }
}

// /surveil.csv (1.6.4): extra surveillance OUIs from the card, "AA:BB:CC,<category id 1-7>" per line, read once
// at boot. Lets the list grow without a reflash; the built-in kSurvOuis still applies first.
SurvOui* g_survExtra = nullptr;
int g_survExtraN = 0;

void loadSurvExtra() {
    File f = SD.open("/surveil.csv", FILE_READ);
    if (!f) return;
    constexpr int kMax = 64;
    g_survExtra = static_cast<SurvOui*>(malloc(kMax * sizeof(SurvOui)));
    if (!g_survExtra) { f.close(); return; }
    char line[48];
    while (f.available() && g_survExtraN < kMax) {
        const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        unsigned a, b, c, cat;
        if (sscanf(line, "%2x:%2x:%2x,%u", &a, &b, &c, &cat) != 4 || cat == 0 || cat >= kSurvNames) continue;
        g_survExtra[g_survExtraN++] = SurvOui{{static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c)},
                                              static_cast<uint8_t>(cat)};
    }
    f.close();
    if (!g_survExtraN) { free(g_survExtra); g_survExtra = nullptr; }
}
