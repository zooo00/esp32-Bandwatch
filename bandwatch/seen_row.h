#pragma once
// /seen.csv v2 - the event log's device register (docs/DEVELOPER.md §20). Pure formatting, parsing and selection
// logic with no Arduino dependency, so it can be compiled natively and tested as is.
//
// One row per device, fixed width so a row can be rewritten in place:
//
//   aa:bb:cc:dd:ee:ff,typ,label (32, space padded)          ,first     ,last      ,count\n
//   0                 18  22                                55         66         77    82
//
// type = "ap " / "sta" / "ble" / "?  " (unknown: rows converted from v1), label = SSID or BLE name at first
// sighting (control characters, ',' and '"' replaced with '.', cut or space-padded to 32 bytes), first / last =
// epoch seconds, 10 digits, 0 when the device had no clock (no "time" from a host yet), count = sessions the
// device was seen in, 5 digits, saturating at 99999. The file starts with kHeader; every '#' line is skipped.
// v1 files held one bare MAC per line ("aa:bb:cc:dd:ee:ff\n").
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace seen {

constexpr int kMacW = 17, kTypeW = 3, kLabelW = 32, kTimeW = 10, kCountW = 5;
constexpr int kOffType = kMacW + 1;                  // 18
constexpr int kOffLabel = kOffType + kTypeW + 1;     // 22
constexpr int kOffFirst = kOffLabel + kLabelW + 1;   // 55
constexpr int kOffLast = kOffFirst + kTimeW + 1;     // 66
constexpr int kOffCount = kOffLast + kTimeW + 1;     // 77
constexpr int kRowLen = kOffCount + kCountW + 1;     // 83 bytes, the newline included
constexpr uint32_t kCountMax = 99999;
constexpr char kHeader[] = "# bandwatch seen v2: mac,type,label,first,last,sessions\n";
constexpr int kHeaderLen = sizeof(kHeader) - 1;
constexpr size_t kLineCap = kRowLen + 8;             // line buffer: a v2 row fits with room to spot a longer line
static_assert(kRowLen == 83, "the v2 row layout is documented as 83 bytes");

enum Kind : uint8_t { KIND_UNKNOWN = 0, KIND_AP = 1, KIND_STA = 2, KIND_BLE = 3 };
enum Line : uint8_t { LINE_BAD = 0, LINE_HEADER, LINE_V1, LINE_V2 };

struct Row {
    uint8_t mac[6];
    uint8_t kind;
    uint32_t first, last, count;
};

inline const char* kindField(uint8_t k) {
    static const char* const kNames[4] = {"?  ", "ap ", "sta", "ble"};
    return kNames[k < 4 ? k : 0];
}

inline void putDec(char* out, uint32_t v, int width) {
    for (int i = width - 1; i >= 0; i--) { out[i] = static_cast<char>('0' + v % 10); v /= 10; }
}

// Hostile input (rule 9): an SSID or BLE name off the air must not be able to add a column or a line.
inline char labelChar(unsigned char c) {
    return (c < 0x20 || c == 0x7f || c == ',' || c == '"') ? '.' : static_cast<char>(c);
}

inline void putLabel(char* out, const char* in) {
    int i = 0;
    if (in) for (; i < kLabelW && in[i]; i++) out[i] = labelChar(static_cast<unsigned char>(in[i]));
    for (; i < kLabelW; i++) out[i] = ' ';
}

inline void putMac(char* out, const uint8_t* m) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3] = kHex[m[i] >> 4];
        out[i * 3 + 1] = kHex[m[i] & 15];
        if (i < 5) out[i * 3 + 2] = ':';
    }
}

// out receives exactly kRowLen bytes (not NUL-terminated).
inline void formatRow(char* out, const uint8_t* mac, uint8_t kind, const char* label, uint32_t first,
                      uint32_t last, uint32_t count) {
    putMac(out, mac);
    out[kOffType - 1] = ',';
    memcpy(out + kOffType, kindField(kind), kTypeW);
    out[kOffLabel - 1] = ',';
    putLabel(out + kOffLabel, label);
    out[kOffFirst - 1] = ',';
    putDec(out + kOffFirst, first, kTimeW);
    out[kOffLast - 1] = ',';
    putDec(out + kOffLast, last, kTimeW);
    out[kOffCount - 1] = ',';
    putDec(out + kOffCount, count > kCountMax ? kCountMax : count, kCountW);
    out[kRowLen - 1] = '\n';
}

// In-place updates patch fields of a row already in a buffer; the caller writes the row back at its offset.
inline void patchSeen(char* row, uint32_t last, uint32_t count) {
    putDec(row + kOffLast, last, kTimeW);
    putDec(row + kOffCount, count > kCountMax ? kCountMax : count, kCountW);
}
inline void patchKind(char* row, uint8_t kind, const char* label) {
    memcpy(row + kOffType, kindField(kind), kTypeW);
    putLabel(row + kOffLabel, label);
}

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool parseMacAt(const char* s, uint8_t* out) {
    for (int i = 0; i < 6; i++) {
        const int hi = hexVal(s[i * 3]), lo = hexVal(s[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[i * 3 + 2] != ':')) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

inline bool parseDec(const char* s, int width, uint32_t& v) {
    uint64_t a = 0;
    for (int i = 0; i < width; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        a = a * 10 + static_cast<uint64_t>(s[i] - '0');
    }
    if (a > 0xFFFFFFFFull) return false;
    v = static_cast<uint32_t>(a);
    return true;
}

inline bool isV2Header(const char* s, size_t n) {
    static const char kTag[] = "# bandwatch seen v2";
    return n >= sizeof(kTag) - 1 && memcmp(s, kTag, sizeof(kTag) - 1) == 0;
}

// One line (without its '\n'; a trailing '\r' is tolerated) -> what it is. n may exceed what s holds when the
// reader had to cut a long line; such a line is never a row. Every pass over the file classifies through here,
// so the passes agree on which lines count.
inline Line classify(const char* s, size_t n, Row& r) {
    while (n && s[n - 1] == '\r') n--;
    if (n && s[0] == '#') return LINE_HEADER;
    if (n == static_cast<size_t>(kMacW) && parseMacAt(s, r.mac)) {
        r.kind = KIND_UNKNOWN;
        r.first = r.last = 0;
        r.count = 1;
        return LINE_V1;
    }
    if (n != static_cast<size_t>(kRowLen - 1) || !parseMacAt(s, r.mac)) return LINE_BAD;
    if (s[kOffType - 1] != ',' || s[kOffLabel - 1] != ',' || s[kOffFirst - 1] != ',' || s[kOffLast - 1] != ',' ||
        s[kOffCount - 1] != ',')
        return LINE_BAD;
    if (!parseDec(s + kOffFirst, kTimeW, r.first) || !parseDec(s + kOffLast, kTimeW, r.last) ||
        !parseDec(s + kOffCount, kCountW, r.count))
        return LINE_BAD;
    r.kind = KIND_UNKNOWN;
    for (uint8_t k = KIND_AP; k <= KIND_BLE; k++)
        if (memcmp(s + kOffType, kindField(k), kTypeW) == 0) r.kind = k;
    return LINE_V2;
}

// Buffered line reader over anything with read(uint8_t*, size_t) (an Arduino File on the device). It tracks the
// file offset of every line, which the in-place update needs, and reads in blocks instead of byte by byte.
template <class Src, size_t N = 256>
class LineReader {
public:
    explicit LineReader(Src* src = nullptr) : src_(src) {}
    void reset(Src* src) { src_ = src; len_ = pos_ = 0; base_ = 0; eof_ = false; }
    // The source's own read position (where to seek back to after writing inside a line already returned).
    uint32_t tell() const { return base_ + static_cast<uint32_t>(len_); }
    // Next line into out (NUL-terminated, at most cap - 1 bytes kept); n = its full length without the '\n' (more
    // than cap - 1 means it was cut); off = file offset of its first byte. False at the end of the input. A last
    // line without a '\n' still counts.
    bool next(char* out, size_t cap, size_t& n, uint32_t& off) {
        n = 0;
        bool any = false;
        off = base_ + static_cast<uint32_t>(pos_);
        for (;;) {
            if (pos_ == len_) {
                if (eof_) break;
                base_ += static_cast<uint32_t>(len_);
                len_ = pos_ = 0;
                const size_t got = src_->read(buf_, N);
                if (got == 0 || got > N) { eof_ = true; break; }
                len_ = got;
            }
            const uint8_t c = buf_[pos_++];
            any = true;
            if (c == '\n') break;
            if (n + 1 < cap) out[n] = static_cast<char>(c);
            n++;
        }
        out[n < cap ? n : cap - 1] = 0;
        return any;
    }

private:
    Src* src_;
    uint8_t buf_[N];
    size_t len_ = 0, pos_ = 0;
    uint32_t base_ = 0;   // file offset of buf_[0]
    bool eof_ = false;
};

// Classify a line the reader returned, treating a cut line as bad.
inline Line classifyRead(const char* s, size_t n, size_t cap, Row& r) {
    return n >= cap ? LINE_BAD : classify(s, n, r);
}

// --- "newest K by last_seen" (rotation and the RAM baseline) ---
// Pass 1 keeps the K largest last_seen values in a min-heap of K uint32 (the RAM baseline array doubles as the heap);
// its root is the threshold. Rows above it are all kept; rows equal to it (ties - every row is a tie when no clock
// was ever set) keep the *last* keepTies of them in file order, i.e. the most recently added, as v1 did.
inline void topkPush(uint32_t* h, int& n, int k, uint32_t v) {
    if (k <= 0) return;
    if (n < k) {
        int i = n++;
        h[i] = v;
        while (i > 0) {
            const int p = (i - 1) / 2;
            if (h[p] <= h[i]) break;
            const uint32_t t = h[p]; h[p] = h[i]; h[i] = t;
            i = p;
        }
        return;
    }
    if (v <= h[0]) return;
    h[0] = v;
    int i = 0;
    for (;;) {
        const int l = 2 * i + 1, r = l + 1;
        int m = i;
        if (l < n && h[l] < h[m]) m = l;
        if (r < n && h[r] < h[m]) m = r;
        if (m == i) break;
        const uint32_t t = h[m]; h[m] = h[i]; h[i] = t;
        i = m;
    }
}

struct Select {
    bool all = true;     // the file holds no more than K rows: keep every one
    uint32_t thr = 0;    // the K-th largest last_seen
    int keepTies = 0;    // how many rows equal to thr are kept
    int ties = 0;        // rows equal to thr in the file (counted by the load pass; the copy pass needs it)
};

inline Select topkSelect(const uint32_t* h, int n, int k) {
    Select s;
    if (n < k) return s;
    s.all = false;
    s.thr = h[0];
    for (int i = 0; i < n; i++) if (h[i] == s.thr) s.keepTies++;
    return s;
}

// Load pass: where the key of a row with this last_seen goes in the K-slot array, or -1 to skip it. Rows above the
// threshold fill [0, K - keepTies) in order; ties go round a ring in [K - keepTies, K), so the last keepTies win.
// Counts s.ties as it goes.
struct RingLoad {
    int k, above = 0, tiesSeen = 0;
    explicit RingLoad(int kk) : k(kk) {}
    int slot(Select& s, uint32_t last) {
        if (s.all) return above < k ? above++ : -1;
        if (last > s.thr) return above < k - s.keepTies ? above++ : -1;
        if (last < s.thr) return -1;
        s.ties++;
        if (s.keepTies <= 0) return -1;
        return (k - s.keepTies) + (tiesSeen++ % s.keepTies);
    }
    int loaded(const Select& s) const {
        if (s.all) return above;
        const int t = tiesSeen < s.keepTies ? tiesSeen : s.keepTies;
        return above + t;
    }
};

// Copy pass (rotation): the same set the load pass kept. tieIdx counts ties seen so far in this pass.
inline bool selectKeep(const Select& s, uint32_t last, int& tieIdx) {
    if (s.all || last > s.thr) return true;
    if (last < s.thr) return false;
    return tieIdx++ >= s.ties - s.keepTies;
}

} // namespace seen
