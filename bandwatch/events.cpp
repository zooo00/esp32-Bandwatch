// Event log to microSD (C4): the untethered "what happened on this walk" record. Two kinds of row go to
// /events.csv - a surveillance-OUI match (first per device per session) and a *new* device, meaning a globally
// unique MAC that is not in the /seen.csv baseline on the same card.
//
// /seen.csv is a device register (v2, format in seen_row.h): one fixed-width row per device with its type, label,
// first / last seen and the number of sessions it was seen in. A new device gets a row appended as it is logged, so
// a device only counts as new once per card; a known device seen again has its row's last_seen / count rewritten in
// place by a sliced pass over the file (regStep), at most every kRegisterMs. v1 files (one MAC per line) are
// converted on attach.
//
// RAM: nothing while off. On, the baseline set (kBaseCap 32-bit entries = 10 KB: a 30-bit MAC hash plus two flag
// bits, "seen since the last register pass" and "counted this session") and the pending-row buffer (~2 KB) live on
// the heap. The card is NOT kept mounted (FATFS is ~30 KB, rule 11): rows buffer here and the card is mounted just
// long enough to append them, every kFlushMs or once kFlushRows are waiting, plus the length of a register pass.
// While sdcap records, the card belongs to the capture: flushes wait (rows keep buffering; overflow is counted).
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
#include <new>
#include "surv_ouis.h"
#include "seen_row.h"

volatile bool g_eventsOn = false;
EvtPending g_evtQ[kEvtQ];
volatile uint8_t evtHead = 0, evtTail = 0;
volatile uint32_t evtQDropped = 0;
EventStats g_evStats;

namespace {
constexpr char kEventsPath[] = "/events.csv";
constexpr char kEventsOld[]  = "/events.old.csv";
constexpr char kSeenPath[]   = "/seen.csv";
constexpr char kSeenOld[]    = "/seen.old.csv";
constexpr char kSeenNew[]    = "/seen.new.csv";   // v1 -> v2 conversion target, swapped in only when complete
constexpr char kSeenBak[]    = "/seen.bak.csv";   // seengen: the real register, put back by "seengen 0"
constexpr char kSeenOldBak[] = "/seen.old.bak.csv";   // seengen: the real /seen.old.csv (a rotation would replace it)
constexpr int kBaseLoad = 2048;            // entries of /seen.csv kept in RAM: the newest by last_seen
// /seen.csv is rotated when an attach finds more than this many rows: twice what is loaded, so the file stays
// <= ~340 KB (83 B a row) and the rewrite (kBaseLoad rows, ~170 KB) is paid at most once per kBaseLoad new devices.
// Older rows move to /seen.old.csv (one generation).
constexpr int kSeenRotate = 2 * kBaseLoad;
constexpr uint32_t kRotateMinHeapB = 24 * 1024;   // a rewrite holds two FATFS files open (~4.3 KB each)
constexpr int kBaseCap = kBaseLoad + 512;  // room for this session's new devices on top
constexpr int kSurvSeenCap = 32;           // surveillance devices already logged this session
constexpr size_t kRowBuf = 2048;           // pending CSV text
constexpr int kNewDevCap = 48;             // pending /seen.csv appends
constexpr int kFlushRows = 16;
constexpr uint32_t kFlushMs = 60000;       // also the cadence of the live-device sweep (touchLive)
constexpr uint32_t kRegisterMs = 300000;   // in-place last_seen / count pass at most this often
constexpr uint32_t kRetryMs = 30000;       // with no usable card: how often to try mounting again
constexpr uint32_t kRotateBytes = 1024UL * 1024UL;
constexpr uint32_t kSeenGenMax = 10000;

// A baseline entry: the MAC's FNV-1a hash with its two low bits replaced by flags. Stealing the bits instead of two
// 320 B bitmaps keeps the register's RAM cost at zero; 30 bits still make a false "known" ~2.4e-6 per lookup.
constexpr uint32_t kKeyMask = ~3u, kDirty = 1u, kCounted = 2u;

struct NewDev { uint8_t mac[6]; uint8_t kind; uint8_t pad; uint32_t epoch; };   // a /seen.csv row waiting to be appended
static_assert(sizeof(NewDev) == 12, "NewDev is 12 B: kNewDevCap of them are on the heap while armed");

using Reader = seen::LineReader<File>;
struct RegPass {             // a running in-place pass (heap, only while it runs): /seen.csv open "r+"
    File f;
    Reader rd;
    uint32_t epoch = 0;      // last_seen written by this pass (0: no clock - last_seen is left as it is)
};

struct EvState {
    uint32_t* base = nullptr;   // sorted entries (key | flags): /seen.csv's newest + this session's new devices
    int baseN = 0;
    int dirtyN = 0;             // entries with kDirty set
    uint32_t survSeen[kSurvSeenCap];
    int survN = 0;
    char* rows = nullptr;       // pending CSV rows
    size_t rowsLen = 0;
    int rowsN = 0;
    NewDev* newDevs = nullptr;
    int newN = 0;
    RegPass* reg = nullptr;
    uint32_t lastFlushMs = 0;
    uint32_t lastTryMs = 0;
    uint32_t lastTouchMs = 0;
    uint32_t lastRegMs = 0;
    bool baseLoaded = false;    // /seen.csv read from the card currently in the slot; novelty is off until then
    bool flushing = false;      // inside flush(): the card's event files are open (sdrm refuses meanwhile)
} ev;

uint32_t macHash32(const uint8_t* m) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ m[i]) * 16777619u;
    return h;
}
inline uint32_t macKey(const uint8_t* m) { return macHash32(m) & kKeyMask; }

uint32_t epochNow() { return epochValid ? epochBase + (millis() - epochBaseMs) / 1000 : 0; }

int baseFind(uint32_t key, bool& found) {   // lower bound on the key bits
    int lo = 0, hi = ev.baseN;
    while (lo < hi) { const int mid = (lo + hi) / 2; if ((ev.base[mid] & kKeyMask) < key) lo = mid + 1; else hi = mid; }
    found = lo < ev.baseN && (ev.base[lo] & kKeyMask) == key;
    return lo;
}

bool baseInsert(uint32_t key, uint32_t flags) {
    bool found;
    const int at = baseFind(key, found);
    if (found) return false;
    if (ev.baseN >= kBaseCap) return true;   // full: still new, just not remembered in RAM (it is still in /seen.csv)
    memmove(ev.base + at + 1, ev.base + at, (ev.baseN - at) * sizeof(uint32_t));
    ev.base[at] = key | flags;
    ev.baseN++;
    return true;
}

// A known device was seen: its row gets last_seen (and, once per session, count + 1) on the next register pass.
void markSeen(uint32_t key) {
    bool found;
    const int i = baseFind(key, found);
    if (found && !(ev.base[i] & kDirty)) { ev.base[i] |= kDirty; ev.dirtyN++; }
}

// Type and label of a device still in the live tables (loop task; one short critical section per table).
bool lookupDev(const uint8_t* mac, bool ble, uint8_t& kind, char* label, size_t cap) {
    bool found = false;
    label[0] = 0;
    if (ble) {
        const int start = macHashIdx(mac, kBleDevSlots);
        portENTER_CRITICAL(&g_devMux);
        for (int p = 0; p < kDevProbe && !found; p++) {
            const BleDev& d = bleDevs[(start + p) % kBleDevSlots];
            if (!d.lastMs || !macEq(d.mac, mac)) continue;
            found = true;
            const size_t n = std::min(strnlen(d.name, sizeof(d.name)), cap - 1);
            memcpy(label, d.name, n);
            label[n] = 0;
        }
        portEXIT_CRITICAL(&g_devMux);
        if (found) kind = seen::KIND_BLE;
    } else {
        const int start = macHashIdx(mac, kWifiDevSlots);
        bool ap = false;
        portENTER_CRITICAL(&g_devMux);
        for (int p = 0; p < kDevProbe && !found; p++) {
            const WifiDev& d = wifiDevs[(start + p) % kWifiDevSlots];
            if (!d.lastMs || !macEq(d.mac, mac)) continue;
            found = true;
            ap = d.flags & 1;
            const size_t n = std::min(strnlen(d.ssid, sizeof(d.ssid)), cap - 1);
            memcpy(label, d.ssid, n);
            label[n] = 0;
        }
        portEXIT_CRITICAL(&g_devMux);
        if (found) kind = ap ? seen::KIND_AP : seen::KIND_STA;
    }
    return found;
}

// Every kFlushMs: mark each known, globally unique device the live tables heard since the last sweep. eventFlag()
// only fires when a slot is created, so without this a device that keeps its slot all session would keep the
// last_seen of its first sighting. RAM only; one table entry per critical section.
void touchLive() {
    const uint32_t since = ev.lastTouchMs;
    ev.lastTouchMs = millis();
    uint8_t mac[6];
    for (int i = 0; i < kWifiDevSlots; i++) {
        portENTER_CRITICAL(&g_devMux);
        const uint32_t last = wifiDevs[i].lastMs;
        memcpy(mac, wifiDevs[i].mac, 6);
        portEXIT_CRITICAL(&g_devMux);
        if (last && static_cast<int32_t>(last - since) > 0 && !(mac[0] & 0x02)) markSeen(macKey(mac));
    }
    for (int i = 0; i < kBleDevSlots; i++) {
        portENTER_CRITICAL(&g_devMux);
        const uint32_t last = bleDevs[i].lastMs;
        const bool random = bleDevs[i].addrType != 0;
        memcpy(mac, bleDevs[i].mac, 6);
        portEXIT_CRITICAL(&g_devMux);
        if (last && static_cast<int32_t>(last - since) > 0 && !random) markSeen(macKey(mac));
    }
}

// A blocking pass over a /seen.csv file on the loop task: every line through the same reader and classifier, so
// all passes (load, selection, conversion, rotation) agree on which lines are rows.
struct Scan {
    Reader rd;
    char line[seen::kLineCap];
    size_t n = 0;
    uint32_t off = 0;
    seen::Row r;
    explicit Scan(File& f) : rd(&f) {}
    bool next(seen::Line& kind) {
        if (!rd.next(line, sizeof(line), n, off)) return false;
        kind = seen::classifyRead(line, n, sizeof(line), r);
        return true;
    }
};
inline bool isRow(seen::Line k) { return k == seen::LINE_V1 || k == seen::LINE_V2; }

// Write one row as v2: a v2 line verbatim, a v1 line converted (type ?, no label, no times, count 1).
bool writeRow(File& out, seen::Line kind, const char* line, const seen::Row& r) {
    char row[seen::kRowLen];
    if (kind == seen::LINE_V2) { memcpy(row, line, seen::kRowLen - 1); row[seen::kRowLen - 1] = '\n'; }
    else seen::formatRow(row, r.mac, seen::KIND_UNKNOWN, "", 0, 0, 1);
    return out.write(reinterpret_cast<const uint8_t*>(row), seen::kRowLen) == static_cast<size_t>(seen::kRowLen);
}
bool writeHeader(File& out) {
    return out.write(reinterpret_cast<const uint8_t*>(seen::kHeader), seen::kHeaderLen) ==
           static_cast<size_t>(seen::kHeaderLen);
}

// v1 (or mixed: an older firmware appended bare MACs to a v2 file) -> v2, every row kept. Written to /seen.new.csv and
// swapped in only when complete; a power cut between the remove and the rename is repaired by the next load.
int convertSeen() {
    if (ESP.getFreeHeap() < kRotateMinHeapB) return -1;
    SD.remove(kSeenNew);
    File in = SD.open(kSeenPath, FILE_READ);
    File out = in ? SD.open(kSeenNew, FILE_WRITE) : File();
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        SD.remove(kSeenNew);
        return -1;
    }
    bool ok = writeHeader(out);
    int rows = 0;
    {
        Scan sc(in);
        seen::Line k;
        while (ok && sc.next(k)) if (isRow(k)) { ok = writeRow(out, k, sc.line, sc.r); rows++; }
    }
    in.close();
    out.close();
    if (!ok || !SD.remove(kSeenPath)) { SD.remove(kSeenNew); return -1; }
    if (!SD.rename(kSeenNew, kSeenPath)) return -1;   // the next load renames it (no /seen.csv, a /seen.new.csv)
    return rows;
}

// /seen.csv holds more than kSeenRotate rows: keep only the newest kBaseLoad by last_seen - exactly the set the RAM
// baseline just loaded (same Select), so novelty is unchanged - and move the whole old file to /seen.old.csv. A copy
// pass with one line buffer; two FATFS files open (skipped below kRotateMinHeapB - retried on the next attach). If
// anything fails after the rename, the old name is put back so the register is never lost; a power cut mid-copy
// leaves /seen.old.csv complete and /seen.csv short (only older history is lost, never the file).
int rotateSeen(const seen::Select& sel) {
    if (ESP.getFreeHeap() < kRotateMinHeapB) return -1;
    SD.remove(kSeenOld);                        // one previous generation, like /events.old.csv
    if (!SD.rename(kSeenPath, kSeenOld)) return -1;
    File in = SD.open(kSeenOld, FILE_READ);
    File out = in ? SD.open(kSeenPath, FILE_WRITE) : File();
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        SD.remove(kSeenPath);
        SD.rename(kSeenOld, kSeenPath);
        return -1;
    }
    bool ok = writeHeader(out);
    int kept = 0, tie = 0;
    {
        Scan sc(in);
        seen::Line k;
        while (ok && sc.next(k)) {
            if (!isRow(k) || !seen::selectKeep(sel, sc.r.last, tie)) continue;
            ok = writeRow(out, k, sc.line, sc.r);
            kept++;
        }
    }
    in.close();
    out.close();
    if (!ok) {   // card full or gone mid-write: put the complete file back under its own name
        SD.remove(kSeenPath);
        SD.rename(kSeenOld, kSeenPath);
        return -1;
    }
    return kept;
}

// Read /seen.csv into the RAM baseline: every row when there are at most kBaseLoad, else the newest kBaseLoad by
// last_seen (a top-K pass, then a load pass). Small files take one pass; the size tells which mode to start in.
// A v1 or mixed file is converted first; a file past kSeenRotate rows is trimmed (rotateSeen) while still mounted.
void loadBaselineImpl(bool mayConvert) {
    const uint32_t t0 = millis();
    ev.baseN = 0;
    ev.dirtyN = 0;
    ev.baseLoaded = true;   // a card without /seen.csv is a valid, empty baseline
    g_evStats.baseFile = 0;
    if (SD.exists(kSeenNew)) {   // a conversion was cut short: finish its swap, or drop the partial copy
        if (SD.exists(kSeenPath)) SD.remove(kSeenNew);
        else SD.rename(kSeenNew, kSeenPath);
    }
    File f = SD.open(kSeenPath, FILE_READ);
    if (!f) return;
    const bool big = f.size() > static_cast<size_t>(seen::kHeaderLen) + static_cast<size_t>(kBaseLoad) * seen::kRowLen;
    int rows = 0, v1 = 0, heapN = 0;
    bool v2hdr = false, firstLine = true;
    {
        Scan sc(f);
        seen::Line k;
        while (sc.next(k)) {
            if (firstLine) { v2hdr = (k == seen::LINE_HEADER && seen::isV2Header(sc.line, sc.n)); firstLine = false; }
            if (!isRow(k)) continue;
            if (k == seen::LINE_V1) v1++;
            if (big) seen::topkPush(ev.base, heapN, kBaseLoad, sc.r.last);
            else if (rows < kBaseLoad) ev.base[rows] = macKey(sc.r.mac);
            rows++;
        }
    }
    f.close();
    if (mayConvert && rows && (v1 || !v2hdr)) {
        const uint32_t tc = millis();
        const int n = convertSeen();
        if (serialRoom(120)) {
            if (n >= 0) Serial.printf("{\"t\":\"log\",\"msg\":\"seen.csv converted to v2: %d rows in %lu ms\"}\n", n,
                                      static_cast<unsigned long>(millis() - tc));
            else Serial.print("{\"t\":\"log\",\"msg\":\"seen.csv v2 conversion failed - kept as is, retried on the next attach\"}\n");
        }
        loadBaselineImpl(false);
        return;
    }
    seen::Select sel;   // all rows, unless there are more than kBaseLoad
    if (big || rows > kBaseLoad) {   // big: pass 1 filled the array with last_seen values, not keys - load again
        if (!big) {     // the size guessed small (short v1 lines): the top-K pass was not run yet
            File g = SD.open(kSeenPath, FILE_READ);
            if (!g) { ev.baseLoaded = false; return; }
            Scan sc(g);
            seen::Line k;
            while (sc.next(k)) if (isRow(k)) seen::topkPush(ev.base, heapN, kBaseLoad, sc.r.last);
            g.close();
        }
        sel = seen::topkSelect(ev.base, heapN, kBaseLoad);
        File g = SD.open(kSeenPath, FILE_READ);
        if (!g) { ev.baseLoaded = false; return; }
        seen::RingLoad ring(kBaseLoad);
        {
            Scan sc(g);
            seen::Line k;
            while (sc.next(k)) {
                if (!isRow(k)) continue;
                const int at = ring.slot(sel, sc.r.last);
                if (at >= 0) ev.base[at] = macKey(sc.r.mac);
            }
        }
        g.close();
        ev.baseN = ring.loaded(sel);
    } else {
        ev.baseN = rows;
    }
    std::sort(ev.base, ev.base + ev.baseN);
    ev.baseN = static_cast<int>(std::unique(ev.base, ev.base + ev.baseN) - ev.base);
    g_evStats.baseFile = rows;
    if (rows > kSeenRotate) {
        const uint32_t tr = millis();
        const int kept = rotateSeen(sel);
        if (kept >= 0) {
            g_evStats.baseFile = kept;
            const uint32_t now = millis();
            if (serialRoom(160))   // V5: how long the rotation and the whole attach kept the loop task busy
                Serial.printf("{\"t\":\"log\",\"msg\":\"seen.csv rotated: %d -> %d in %lu ms (attach %lu ms)\"}\n", rows,
                              kept, static_cast<unsigned long>(now - tr), static_cast<unsigned long>(now - t0));
        }
    }
}
void loadBaseline() { loadBaselineImpl(true); }

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
    // n is the length snprintf *wanted*: a row it had to cut short (n >= sizeof(row)) is dropped, not copied past
    // the end of row. Today's rows are well under 128 B; this keeps the memcpy honest if a field ever grows.
    if (n <= 0 || n >= static_cast<int>(sizeof(row)) || ev.rowsLen + n > kRowBuf) { g_evStats.dropped++; return; }
    memcpy(ev.rows + ev.rowsLen, row, n);
    ev.rowsLen += n;
    ev.rowsN++;
    g_evStats.pending = ev.rowsN;
}

// Close a running register pass without touching the mount (the caller decides about that). Entries it had not
// reached keep their kDirty bit and go out with the next pass.
void regClose() {
    if (!ev.reg) return;
    ev.reg->f.close();
    delete ev.reg;
    ev.reg = nullptr;
}

void cardLost() {
    regClose();
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
        // Devices first seen while the card was out are still waiting in newDevs (they go to /seen.csv on this
        // flush); put them back in the RAM set, or the reload would let them count as new a second time.
        for (int i = 0; i < ev.newN; i++) baseInsert(macKey(ev.newDevs[i].mac), kCounted);
    }
    g_evStats.cardOk = true;
    return true;
}

// Start the in-place register pass: /seen.csv opened "r+" and kept open (card mounted) until regStep() reaches the
// end. Needs the same heap margin as a rotation; with less, the dirty bits simply wait for the next try.
bool regStart() {
    if (ev.reg || !ev.dirtyN || !ev.baseLoaded || ESP.getFreeHeap() < kRotateMinHeapB) return false;
    RegPass* p = new (std::nothrow) RegPass();
    if (!p) return false;
    p->f = SD.open(kSeenPath, "r+");
    if (!p->f) { delete p; return false; }
    p->rd.reset(&p->f);
    p->epoch = epochNow();
    ev.reg = p;
    return true;
}

// One slice of the register pass (loop task, at most kSdBudgetUs - the capture drain's budget - so hopping and the
// LCD keep running). Each row whose entry is dirty gets last_seen = now (kept when there is no clock), count + 1 the
// first time this session, and - for a v1 row of unknown type still in the live tables - its type and label. The row
// is patched in the line buffer and written back over itself (same width), then the read position is restored.
// Duplicate rows of one MAC (possible after a v1 history): only the first one in the file is updated.
void regStep() {
    RegPass* p = ev.reg;
    if (!p) return;
    if (sd.capEnabled || sd.readActive) { regClose(); return; }   // they own the mount now; the bits wait
    const uint32_t t0 = micros();
    char line[seen::kLineCap];
    size_t n;
    uint32_t off;
    seen::Row r;
    while (micros() - t0 < kSdBudgetUs) {
        if (!p->rd.next(line, sizeof(line), n, off)) { regClose(); sdUnmount(); return; }   // done (or a short read)
        if (seen::classifyRead(line, n, sizeof(line), r) != seen::LINE_V2) continue;
        bool found;
        const int i = baseFind(macKey(r.mac), found);
        if (!found || !(ev.base[i] & kDirty)) continue;
        if (r.kind == seen::KIND_UNKNOWN) {
            uint8_t kind = seen::KIND_UNKNOWN;
            char label[seen::kLabelW + 2];
            if (lookupDev(r.mac, false, kind, label, sizeof(label)) || lookupDev(r.mac, true, kind, label, sizeof(label)))
                seen::patchKind(line, kind, label);
        }
        const bool firstThisSession = !(ev.base[i] & kCounted);
        seen::patchSeen(line, p->epoch > r.last ? p->epoch : r.last, firstThisSession ? r.count + 1 : r.count);
        constexpr size_t kBody = seen::kRowLen - 1;   // the row without its newline
        if (!p->f.seek(off) || p->f.write(reinterpret_cast<const uint8_t*>(line), kBody) != kBody || !p->f.seek(p->rd.tell())) {
            cardLost();   // closes the pass first
            return;
        }
        ev.base[i] = (ev.base[i] & ~kDirty) | kCounted;
        ev.dirtyN--;
    }
}

// Mount, append, rotate, start the register pass when due, unmount. Returns false (rows kept) when the card is busy
// or missing. forceReg: start the register pass even if kRegisterMs has not passed (events 0).
bool flush(bool forceReg = false) {
    if (ev.reg) return false;   // the register pass holds /seen.csv: rows wait a few loops for it
    const uint32_t now = millis();
    const bool regDue = ev.dirtyN && (forceReg || now - ev.lastRegMs >= kRegisterMs);
    if (regDue) ev.lastRegMs = now;   // tried now, whatever happens: never a mount attempt every loop
    if (!ev.rowsN && !ev.newN && !regDue) return true;
    if (sd.capEnabled || sd.readActive) return false;   // the card belongs to the capture / sdread right now
    const bool wasMounted = sd.mounted;
    if (!attachCard()) return false;
    struct FlushMark { FlushMark() { ev.flushing = true; } ~FlushMark() { ev.flushing = false; } } mark;
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
            if (f.size() == 0) ok = writeHeader(f);
            char row[seen::kRowLen], label[seen::kLabelW + 2];
            for (int i = 0; ok && i < ev.newN; i++) {
                NewDev& d = ev.newDevs[i];
                // Label (and AP vs station) from the live table now: a beacon or scan response has usually arrived
                // since the slot was created. An evicted slot leaves the type known at creation and no label.
                if (!lookupDev(d.mac, d.kind == seen::KIND_BLE, d.kind, label, sizeof(label))) label[0] = 0;
                seen::formatRow(row, d.mac, d.kind, label, d.epoch, d.epoch, 1);
                ok = f.write(reinterpret_cast<const uint8_t*>(row), seen::kRowLen) == static_cast<size_t>(seen::kRowLen);
            }
            f.close();
            if (ok) { g_evStats.baseFile += ev.newN; ev.newN = 0; }
        }
    }
    if (!ok) { cardLost(); return false; }   // pulled mid-write: rows stay buffered for the next card
    if (!(regDue && regStart()) && !wasMounted) sdUnmount();   // a started pass keeps the mount until it ends
    g_evStats.pending = ev.rowsN;
    ev.lastFlushMs = millis();
    return ok;
}

void freeAll() {
    regClose();
    free(ev.base); ev.base = nullptr;
    free(ev.rows); ev.rows = nullptr;
    free(ev.newDevs); ev.newDevs = nullptr;
    ev.baseN = ev.survN = ev.newN = ev.rowsN = ev.dirtyN = 0;
    ev.rowsLen = 0;
}

// The file under /seen.csv was replaced (seengen): load it like an attach, keeping this session's pending appends.
void reloadBaseline() {
    loadBaseline();
    for (int i = 0; i < ev.newN; i++) baseInsert(macKey(ev.newDevs[i].mac), kCounted);
    g_evStats.cardOk = true;
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
    ev.newDevs = static_cast<NewDev*>(malloc(kNewDevCap * sizeof(NewDev)));
    if (!ev.base || !ev.rows || !ev.newDevs) { freeAll(); return false; }
    ev.survN = ev.newN = ev.rowsN = ev.dirtyN = 0;
    ev.rowsLen = 0;
    ev.baseLoaded = false;
    ev.lastFlushMs = ev.lastTryMs = ev.lastTouchMs = ev.lastRegMs = millis();
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
    // Best effort, blocking (bounded by one read of /seen.csv): finish a running register pass, flush, and write
    // this session's remaining sightings, so a disarm leaves the register current. A busy card keeps the rows only
    // until we free them, so say so in the counters.
    while (ev.reg) regStep();
    touchLive();
    flush(true);
    while (ev.reg) regStep();
    if (ev.rowsN) g_evStats.dropped += ev.rowsN;
    freeAll();
    g_evStats.pending = 0;
}

int eventsBaseCount() { return ev.baseN; }

// sdrm support (sd_sink.cpp sdRemoveFile). flush() opens /events.csv and /seen.csv between attachCard() and the
// unmount, and a register pass keeps /seen.csv open across loops until it ends; deleting either file then would race
// the log, so sdrm (and seengen) refuse meanwhile.
bool eventsFlushing() { return ev.flushing || ev.reg != nullptr; }

// sdUnmount() keeps the card mounted while a register pass has /seen.csv open (sd_sink.cpp).
bool eventsHoldsCard() { return ev.reg != nullptr; }

// sdcap / sdread are about to use the card: a running register pass steps aside (its unreached rows stay dirty and
// are written by the next pass). The mount stays: the caller is about to use it and unmounts as usual.
void eventsReleaseCard() { regClose(); }

// A card file was just deleted (card mounted, loop task). Deleting /seen.csv while the log is armed means "start
// novelty over": reload the baseline from the card - now empty - instead of keeping the RAM set, which would go on
// treating every device of the old baseline as known. Devices still waiting to be appended (newDevs) belonged to the
// deleted baseline and are dropped with it; their "new" rows are already in /events.csv. Deleting /events.csv
// needs nothing: the next flush creates it again with its header (rows still buffered land there).
void eventsFileRemoved(const char* path) {
    if (!g_eventsOn || strcmp(path, kSeenPath) != 0) return;
    ev.newN = 0;
    loadBaseline();          // no file: baseLoaded = true, baseN = 0, baseFile = 0
    g_evStats.cardOk = true; // the card answered the delete
}

// A card just appeared: attach on the next serviceEvents() pass instead of waiting out kRetryMs.
void eventsNudge() { ev.lastTryMs = millis() - kRetryMs; }

// "seengen <n>" (diagnostic, BACKLOG V5): replace /seen.csv with n synthetic v2 rows - globally unique MACs
// 00:1b:63:xx:xx:xx, types and labels varied, first/last spread over 30 days - so the next attach has a rotation to do
// and logs how long it took. The real register is parked first: /seen.csv -> /seen.bak.csv (an empty one when there
// was none, so "nothing" is restored as nothing) and /seen.old.csv -> /seen.old.bak.csv, because the rotation would
// replace /seen.old.csv. Parked files are never overwritten: a second seengen only replaces the synthetic file.
// "seengen 0" deletes the synthetic files and puts both back. Blocking on the loop task (~1 s per few thousand rows,
// yielding every 64 rows); refused while sdcap / sdread / an event-log flush or register pass has the card. While the
// log is armed the new file is loaded at once (an attach: a rotation logs right after the ack).
void seenGenerate(long n) {
    if (n < 0 || n > static_cast<long>(kSeenGenMax)) {
        sendLinef("{\"t\":\"err\",\"msg\":\"seengen: n is 1..%lu, or 0 to restore /seen.bak.csv\"}\n",
                  static_cast<unsigned long>(kSeenGenMax));
        return;
    }
    if (sd.capEnabled || sd.readActive || eventsFlushing()) {
        sendLinef("{\"t\":\"err\",\"msg\":\"seengen: busy - sdcap, sdread or the event log has the card\"}\n");
        return;
    }
    if (!sdMount()) { sendLinef("{\"t\":\"err\",\"msg\":\"seengen: no card\"}\n"); return; }
    const uint32_t t0 = millis();
    bool ok = true;
    if (n == 0) {
        if (!SD.exists(kSeenBak)) {
            sendLinef("{\"t\":\"err\",\"msg\":\"seengen: no /seen.bak.csv to restore\"}\n");
            sdUnmount();
            return;
        }
        SD.remove(kSeenPath);
        ok = SD.rename(kSeenBak, kSeenPath);
        SD.remove(kSeenOld);   // the synthetic rotation's output
        if (SD.exists(kSeenOldBak)) ok = SD.rename(kSeenOldBak, kSeenOld) && ok;
    } else {
        if (SD.exists(kSeenBak)) {                       // parked by an earlier run: the current files are synthetic
            if (SD.exists(kSeenPath)) ok = SD.remove(kSeenPath);
        } else {
            if (SD.exists(kSeenPath)) ok = SD.rename(kSeenPath, kSeenBak);
            else { File e = SD.open(kSeenBak, FILE_WRITE); ok = static_cast<bool>(e); if (e) e.close(); }
            if (ok && SD.exists(kSeenOld)) { SD.remove(kSeenOldBak); ok = SD.rename(kSeenOld, kSeenOldBak); }
        }
        File f = ok ? SD.open(kSeenPath, FILE_WRITE) : File();
        ok = f && writeHeader(f);
        const uint32_t now = epochValid ? epochNow() : 1790000000u;   // no clock: a fixed date in 2026
        constexpr uint32_t kSpanS = 30u * 86400u;
        uint32_t x = 0x9E3779B9u ^ static_cast<uint32_t>(n);
        auto rnd = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
        char row[seen::kRowLen], label[20];
        for (long i = 0; ok && i < n; i++) {
            const uint8_t mac[6] = {0x00, 0x1b, 0x63, static_cast<uint8_t>(i >> 16), static_cast<uint8_t>(i >> 8),
                                    static_cast<uint8_t>(i)};
            const uint8_t kind = static_cast<uint8_t>(1 + i % 3);   // ap, sta, ble
            if (kind == seen::KIND_STA) label[0] = 0;
            else snprintf(label, sizeof(label), "%s-%05ld", kind == seen::KIND_AP ? "net" : "tag", i);
            const uint32_t last = now - rnd() % kSpanS;
            const uint32_t first = last - rnd() % kSpanS;
            seen::formatRow(row, mac, kind, label, first, last, 1 + rnd() % 20);
            ok = f.write(reinterpret_cast<const uint8_t*>(row), seen::kRowLen) == static_cast<size_t>(seen::kRowLen);
            if ((i & 63) == 63) delay(1);   // let the idle task run (task watchdog)
        }
        if (f) f.close();
    }
    sendLinef("{\"t\":\"ack\",\"cmd\":\"seengen\",\"n\":%ld,\"ms\":%lu,\"ok\":%d}\n", n,
              static_cast<unsigned long>(millis() - t0), ok ? 1 : 0);
    if (g_eventsOn && ok) reloadBaseline();
    sdUnmount();
}

// Loop side: classify what the radios flagged, buffer rows, flush on size or time, run the register pass.
void serviceEvents() {
    if (!g_eventsOn) return;
    if (ev.reg) regStep();
    for (;;) {
        EvtPending e;
        portENTER_CRITICAL(&g_devMux);
        const bool have = evtTail != evtHead;
        if (have) { e = g_evtQ[evtTail]; evtTail = static_cast<uint8_t>((evtTail + 1) % kEvtQ); }
        portEXIT_CRITICAL(&g_devMux);
        if (!have) break;
        const uint32_t key = macKey(e.mac);
        if (e.surv) {
            bool seen = false;
            for (int i = 0; i < ev.survN; i++) if (ev.survSeen[i] == key) { seen = true; break; }
            if (!seen) {
                if (ev.survN < kSurvSeenCap) ev.survSeen[ev.survN++] = key;
                char extra[40];
                snprintf(extra, sizeof(extra), "%s%s", kSurvName[e.surv < kSurvNames ? e.surv : 0],
                         (e.flags & EVF_TIER1) ? " (dest only)" : "");
                addRow("surv", e, extra);
                g_evStats.surv++;
            }
        }
        if (e.flags & EVF_RANDOM) continue;   // surveillance logged above; randomized MACs never count as new
        if (!ev.baseLoaded) { g_evStats.waiting++; continue; }   // no card yet: cannot tell new from known
        if (baseInsert(key, kCounted)) {
            addRow("new", e, (e.flags & EVF_TIER1) ? "dest only" : "");
            g_evStats.fresh++;
            ledAlertNote(LED_ALERT_NEW);   // D1: less urgent blip than surveillance (rate-limited in led_alert.cpp)
            if (ev.newN < kNewDevCap) {
                NewDev& d = ev.newDevs[ev.newN++];
                memcpy(d.mac, e.mac, 6);
                d.kind = (e.flags & EVF_BLE) ? seen::KIND_BLE : seen::KIND_STA;
                char label[seen::kLabelW + 2];
                lookupDev(d.mac, e.flags & EVF_BLE, d.kind, label, sizeof(label));   // AP vs station, if known yet
                d.pad = 0;
                d.epoch = epochNow();
            }
        } else {
            markSeen(key);   // known: last_seen / count on the next register pass
        }
    }
    g_evStats.qDropped = evtQDropped;
    const uint32_t now = millis();
    if (ev.baseLoaded && now - ev.lastTouchMs >= kFlushMs) touchLive();
    // No usable card: try again every kRetryMs (an SD.begin with no card blocks the loop briefly, so not every pass).
    if (!ev.baseLoaded && !ev.reg && !sd.capEnabled && !sd.readActive && now - ev.lastTryMs >= kRetryMs) {
        ev.lastTryMs = now;
        const bool wasMounted = sd.mounted;
        if (attachCard()) {
            flush();   // rows that waited for a card go out now
            if (!wasMounted) sdUnmount();   // kept while a register pass started by that flush runs
        }
    }
    const bool regDue = ev.dirtyN && !ev.reg && now - ev.lastRegMs >= kRegisterMs;
    if (ev.rowsN >= kFlushRows || ev.newN >= kNewDevCap || regDue ||
        ((ev.rowsN || ev.newN) && now - ev.lastFlushMs >= kFlushMs)) {
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
        int end = 0;
        if (sscanf(line, "%2x:%2x:%2x,%u%n", &a, &b, &c, &cat, &end) != 4 || cat == 0 || cat >= kSurvNames) continue;
        while (line[end] == ' ' || line[end] == '\t' || line[end] == '\r') end++;   // CRLF files and trailing blanks are fine
        if (line[end]) continue;                   // anything else after the category: not a line we wrote the format for
        g_survExtra[g_survExtraN++] = SurvOui{{static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c)},
                                              static_cast<uint8_t>(cat)};
    }
    f.close();
    if (!g_survExtraN) { free(g_survExtra); g_survExtra = nullptr; }
}
