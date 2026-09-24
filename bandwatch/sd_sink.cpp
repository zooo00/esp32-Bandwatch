// microSD pcap sink for Bandwatch: byte-compatible with PcapWriter in host/bandwatch_host.py (same global
// header, same radiotap Wi-Fi link type 127 and 802.15.4-TAP link type 283 per-frame headers), so a file
// written here and one written by the host are interchangeable. The card shares the LCD's SPI bus (CS GPIO4);
// both run on the loop task, every transfer wrapped in beginTransaction/endTransaction - never touch the card
// from a radio callback or another task (docs/DEVELOPER.md §12). FATFS is mounted only while in use: it costs
// ~30 KB and BLE mode cuts its scans short below ~28 KB free.
#include "bandwatch_core.h"
#include <SPI.h>
#include <SD.h>
#include <time.h>

constexpr int kSdCsPin = 4;
constexpr uint32_t kSdSpiHz = 20000000;       // SD over SPI; the LCD runs the same bus at 40 MHz
constexpr size_t kSdBufSize = 4096;           // == CONFIG_FATFS_SECTOR_4096
constexpr uint32_t kSdFlushMs = 5000;         // fsync cadence: a power cut costs at most this much capture

SdSink sd;   // the sink's state (type in bandwatch_core.h)

namespace {

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

} // namespace

// Mounting FATFS costs ~30 KB of heap, and BLE mode cuts its scans short below ~28 KB free, so the card is
// mounted only while it is being used and released again afterwards. sd.cardPresent/sd.cardMb remember what
// the boot probe found so the dashboard can still show the card without paying for it.
bool sdMount() {
    if (sd.mounted) return true;
    sd.mounted = SD.begin(kSdCsPin, SPI, kSdSpiHz);
    if (sd.mounted) { sd.cardPresent = true; sd.cardMb = static_cast<uint32_t>(SD.cardSize() / (1024 * 1024)); }
    return sd.mounted;
}

void sdUnmount() {
    if (!sd.mounted || sd.capEnabled || sd.readActive) return;   // never pull the filesystem out from under
                                                              // an open capture or an in-flight sdread
    SD.end();
    sd.mounted = false;
}

void sdProbeAtBoot() {
    if (sdMount()) sdUnmount();
}

namespace {

bool sdWriteRaw(const uint8_t* d, size_t n) {
    if (sd.file.write(d, n) != n) { sd.errors++; return false; }
    sd.bytes += n;
    return true;
}

bool sdBufFlush() {
    if (!sd.bufLen) return true;
    const bool ok = sdWriteRaw(sd.buf, sd.bufLen);
    sd.bufLen = 0;
    return ok;
}

bool sdBufPut(const uint8_t* d, size_t n) {
    while (n) {
        const size_t room = kSdBufSize - sd.bufLen;
        const size_t take = n < room ? n : room;
        memcpy(sd.buf + sd.bufLen, d, take);
        sd.bufLen += take; d += take; n -= take;
        if (sd.bufLen == kSdBufSize && !sdBufFlush()) return false;
    }
    return true;
}

} // namespace

void sdCloseCapture() {
    if (!sd.capEnabled) return;
    sd.capEnabled = false;
    sdBufFlush();
    if (sd.file) { sd.file.flush(); sd.file.close(); }
    if (sd.buf) { free(sd.buf); sd.buf = nullptr; }
    sd.bufLen = 0;
    if (serialRoom(160))
        Serial.printf("{\"t\":\"log\",\"msg\":\"sd capture closed: %s (%lu frames, %lu bytes)\"}\n",
                      sd.path, static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes));
    sdUnmount();   // hand the ~30 KB back
}

bool sdOpenCapture() {
    if (sd.capEnabled) return true;
    if (!sdMount()) return false;
    sd.buf = static_cast<uint8_t*>(malloc(kSdBufSize));
    if (!sd.buf) return false;
    sd.bufLen = 0; sd.frames = 0; sd.bytes = 0; sd.dropped = 0; sd.errors = 0;
    const bool is154 = mode154();
    const bool isBle = (bandMode == BAND_BLE);
    if (epochValid) {   // host gave us a clock: name the file after it, like the host tool does
        uint32_t s, us; nowEpoch(s, us);
        const time_t t = static_cast<time_t>(s);
        struct tm tmv; gmtime_r(&t, &tmv);
        snprintf(sd.path, sizeof(sd.path), "/bandwatch-%s-%04d%02d%02d-%02d%02d%02d.pcap",
                 is154 ? "802154" : isBle ? "ble" : "wifi", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {            // no clock: fall back to a counter so files never collide
        // Resume from the last index used this session instead of rescanning from 1: on a card with many
        // captures that was up to 10000 FATFS lookups, all of them on the loop task that also hops channels.
        static int nextSeq = 1;
        int i = nextSeq;
        for (; i < 10000; i++) {
            snprintf(sd.path, sizeof(sd.path), "/bandwatch-%s-%04d.pcap", is154 ? "802154" : isBle ? "ble" : "wifi", i);
            if (!SD.exists(sd.path)) break;
        }
        nextSeq = i + 1;
    }
    sd.file = SD.open(sd.path, FILE_WRITE);
    if (!sd.file) { free(sd.buf); sd.buf = nullptr; return false; }
    uint8_t gh[24];                                  // pcap global header, little endian
    putLE32(gh + 0, 0xA1B2C3D4); putLE16(gh + 4, 2); putLE16(gh + 6, 4);
    putLE32(gh + 8, 0); putLE32(gh + 12, 0); putLE32(gh + 16, 65535);
    putLE32(gh + 20, is154 ? 283 : isBle ? 256 : 127);   // 802.15.4-TAP / BLE LL w/ phdr / radiotap
    if (!sdBufPut(gh, sizeof(gh))) { sdCloseCapture(); return false; }
    sd.capEnabled = true;
    sd.lastFlushMs = millis();
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
    sd.frames++;
    return true;
}

namespace {

// Pull a capture off the card without ejecting it: "S <n> <base64>" lines, bracketed by an ack.
// Driven incrementally from Bandwatch_Loop with a time budget, NOT in one blocking loop inside the
// command handler: that shares the loop task with lv_timer_handler -> hopIfNeeded(), so a blocking read
// of a 1.6 MB capture stalled channel hopping for seconds and skewed every dwell in that window.
File sdReadFh;
uint32_t sdReadSent = 0, sdReadTotal = 0;

} // namespace

void sdReadAbort() {
    if (!sd.readActive) return;
    sd.readActive = false;
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
    sd.readActive = true;
    Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdread\",\"file\":\"%s\",\"bytes\":%lu}\n", path,
                  static_cast<unsigned long>(sdReadTotal));
}

// Called every loop: emit what fits in the TX buffer, then yield so hopping and the UI keep running.
void serviceSdRead() {
    if (!sd.readActive) return;
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
    sd.readActive = false;
    sdReadFh.close();
    sdUnmount();
}

void sdListFiles() {
    if (!sdMount()) { Serial.print("{\"t\":\"err\",\"msg\":\"sdls: no card\"}\n"); return; }
    File root = SD.open("/");
    if (!root) { Serial.print("{\"t\":\"err\",\"msg\":\"sdls: cannot open /\"}\n"); return; }
    // "sent" < "total": the serial buffer filled up mid-list (host slow or absent), so entries were dropped.
    // The host shows what arrived and knows there is more on the card.
    int total = 0, sent = 0;
    Serial.print("{\"t\":\"sdls\",\"files\":[");
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        if (!e.isDirectory()) {
            total++;
            if (serialRoom(120)) {
                Serial.printf("%s[\"%s\",%lu]", sent ? "," : "", e.name(), static_cast<unsigned long>(e.size()));
                sent++;
            }
        }
        e.close();
    }
    root.close();
    Serial.printf("],\"total\":%d,\"sent\":%d}\n", total, sent);
    sdUnmount();
}

void sdServiceFlush() {
    if (!sd.capEnabled) return;
    const uint32_t now = millis();
    if (now - sd.lastFlushMs < kSdFlushMs) return;
    sd.lastFlushMs = now;
    sdBufFlush();
    sd.file.flush();     // push FAT metadata so the file stays valid if power is lost
}
