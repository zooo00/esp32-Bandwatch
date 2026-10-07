// Capture ring for Bandwatch: one heap-allocated buffer both sinks pull frames from - the USB serial sink
// gets a base64 "P ..." line per frame, the SD sink writes byte-compatible pcap (see sd_sink.cpp). The ring
// is sized against free heap on first use and handed back when no sink wants frames anymore; see
// docs/DEVELOPER.md §16 for the RAM budget.
#include "bandwatch_core.h"

constexpr uint32_t kCapHeapReserve = 14000;   // keep this much heap free when sizing the ring
// Floor for TOTAL free heap once the ring is allocated: below it, LVGL page rebuilds and SD writes start
// failing mid-capture instead of the capture being refused up front. Sized against the tightest normal state
// (SD recording on top of a full 20-slot ring; docs/DEVELOPER.md §16).
constexpr uint32_t kMinFreeHeapB = 24 * 1024;

// Single-producer (Wi-Fi task, NimBLE host task, 802.15.4 ISR) / single-consumer (loop) ring buffer for
// captured frames. Allocated from the heap only while a capture runs (~32 KB), so it costs nothing otherwise.
CapFrame* capRing = nullptr;
int capSlots = 0;
volatile uint8_t capHead = 0;     // next slot the producer writes
volatile uint8_t capTail = 0;     // next slot the consumer reads
bool trackAddr1 = true;           // tier-1 receiver-side sightings (see trackWifiDevice)
volatile bool captureEnabled = false;   // USB sink
volatile bool capActive = false;        // either sink wants frames: the RX paths gate on this
volatile uint16_t capSnapLen = kCapMaxLen;
volatile uint32_t capDropped = 0;
uint32_t capSent = 0;             // frames streamed to the USB sink since the ring was allocated

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

// Both sinks share one ring; allocate it on first use and keep it until every sink is done.
bool ensureCapRing() {
    if (capRing) return true;
    capDropped = 0; capSent = 0; capHead = 0; capTail = 0;
    const uint32_t freeHeap = ESP.getMaxAllocHeap();
    int slots = (freeHeap > kCapHeapReserve) ? static_cast<int>((freeHeap - kCapHeapReserve) / sizeof(CapFrame)) : 0;
    if (slots > kCapSlotsMax) slots = kCapSlotsMax;
    // Also leave the floor below standing: a smaller ring beats refusing outright when heap is tight.
    const uint32_t totalFree = ESP.getFreeHeap();
    const int floorSlots = (totalFree > kMinFreeHeapB) ? static_cast<int>((totalFree - kMinFreeHeapB) / sizeof(CapFrame)) : 0;
    if (slots > floorSlots) slots = floorSlots;
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
    // Heap floor: the ring is sized against the largest block, but total free heap decides whether a page
    // rebuild or SD write can still succeed while it lives. Refuse and hand it back rather than OOM later.
    if (ESP.getFreeHeap() < kMinFreeHeapB) {
        const uint32_t after = ESP.getFreeHeap();
        releaseCapture();
        if (serialRoom(160))
            Serial.printf("{\"t\":\"err\",\"msg\":\"capture refused: %u B free after ring, want >= %u\"}\n",
                          static_cast<unsigned>(after), static_cast<unsigned>(kMinFreeHeapB));
        return false;
    }
    if (serialRoom(100)) Serial.printf("{\"t\":\"log\",\"msg\":\"capture ring: %d slots\"}\n", capSlots);
    return true;
}

// Stop capturing and give the ring back to the heap. Safe from the loop task only: the radio callbacks
// re-read capRing/captureEnabled on every frame and run to completion, so they never hold a stale pointer.
namespace {
// Free only the buffer; the sink flags are the caller's business. Clear capActive first so no producer
// reserves a slot in it.
void freeCapRing() {
    if (!capRing) return;
    CapFrame* r = capRing;
    capRing = nullptr;
    capSlots = 0;
    capHead = 0;      // separate stores: chaining them reads back a volatile, which C++20 deprecates
    capTail = 0;
    free(r);
}
} // namespace

void releaseCapture() {
    captureEnabled = false;
    sdCloseCapture();
    capActive = false;
    freeCapRing();
}

// ensureCapRing() for a caller that just took heap away (sdcap mounting FATFS): a ring that already exists
// was sized and floor-checked against the heap *before* that, so re-size it against what is left now rather
// than sit below kMinFreeHeapB. Both sinks stay enabled; the frames still in the old ring are lost. On
// failure the whole capture is released (USB sink included) and false returned.
bool refitCapRing() {
    if (!capRing || ESP.getFreeHeap() >= kMinFreeHeapB) return ensureCapRing();
    const uint32_t before = ESP.getFreeHeap();
    capActive = false;
    freeCapRing();
    if (!ensureCapRing()) { releaseCapture(); return false; }
    capActive = captureEnabled || sd.capEnabled;
    if (serialRoom(120))
        Serial.printf("{\"t\":\"log\",\"msg\":\"capture ring re-sized to %d slots (heap was %u)\"}\n", capSlots,
                      static_cast<unsigned>(before));
    return true;
}

// Recompute what the RX paths should do, and hand the ring back once no sink wants frames.
void syncCapActive() {
    capActive = captureEnabled || sd.capEnabled;
    if (!capActive) releaseCapture();
}

// The one ring-access discipline for all three producers (Wi-Fi task, NimBLE host task, 802.15.4 ISR):
// reserve a slot, fill it, commit so the tail only advances on commit. A slow consumer sees a full ring;
// we count the drop and keep the radio going.
CapFrame* capReserve(uint8_t& nextHead) {
    if (!capActive || !capRing) return nullptr;
    const uint8_t head = capHead;
    nextHead = static_cast<uint8_t>((head + 1) % capSlots);
    if (nextHead == capTail) {
        capDropped = capDropped + 1;   // consumer too slow: drop, but count it
        return nullptr;
    }
    return &capRing[head];
}

void capCommit(uint8_t nextHead) {
    capHead = nextHead;
}

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
        if (usbWant && !usbRoom && !sd.capEnabled) return;   // USB-only: stall rather than lose the frame
        if (usbWant && usbRoom) {
            Serial.printf("P %u %d %lu %u ", f.channel, f.rssi, static_cast<unsigned long>(f.ts_us), f.len);
            writeBase64(f.data, f.capLen);
            Serial.write('\n');
            capSent += 1;
        } else if (usbWant) {
            capDropped = capDropped + 1;                    // SD is recording: never block the card on USB
        }
        if (sd.capEnabled && !sdWriteFrame(f)) { sd.dropped++; sdCloseCapture(); }
        capTail = (capTail + 1) % capSlots;
        // SD writes can stall for tens of ms on card GC; hopIfNeeded() shares this task, so cap the time.
        if (sd.capEnabled && (micros() - started) > kSdBudgetUs) break;
    }
    sdServiceFlush();
}
