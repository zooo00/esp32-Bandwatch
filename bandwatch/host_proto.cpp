// Host protocol for Bandwatch: one JSON object per line over USB serial; captured frames stream as
// "P ..." base64 lines (the host merges them into a pcap). Line shapes must stay in sync with
// handle_line/merge_* in host/bandwatch_host.py and docs/DEVELOPER.md. Every send checks serialRoom()
// first so a slow or absent host drops whole lines, never half of one (rule 6): streamed lines against a
// worst-case budget derived next to them, short ack/err lines through sendLinef().
#include "bandwatch_core.h"
#include "LVGL_Driver.h"   // LCD_WIDTH/LCD_HEIGHT for the mirror ack
#include <esp_wifi.h>   // C API for the txtest branch (promiscuous on/off, channel)
#include <SD.h>     // the sdinfo branch reports card size while mounted
#include <stdarg.h>
#include <string.h>

bool sendLinef(const char* fmt, ...) {
    char buf[320];   // the longest ack is sdread's with a fully escaped 40-char path (~300 B)
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf) || !serialRoom(static_cast<size_t>(n))) return false;
    Serial.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
    return true;
}

// Escaped size of one character inside a JSON string: \" and \\ take 2, other control bytes a 6-byte \u escape.
static inline size_t jsonCharLen(unsigned char c) { return (c == '"' || c == '\\') ? 2 : (c < 0x20) ? 6 : 1; }

size_t jsonStrLen(const char* s) {
    size_t n = 2;   // the quotes
    for (; *s; s++) n += jsonCharLen(static_cast<unsigned char>(*s));
    return n;
}

bool jsonQuote(char* out, size_t n, const char* s) {
    if (!n) return false;
    out[0] = 0;
    if (jsonStrLen(s) + 1 > n) return false;
    size_t o = 0;
    out[o++] = '"';
    for (; *s; s++) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = static_cast<char>(c); }
        else if (c < 0x20) { snprintf(out + o, 7, "\\u%04x", c); o += 6; }
        else out[o++] = static_cast<char>(c);
    }
    out[o++] = '"';
    out[o] = 0;
    return true;
}

void fmtMac(char* out, size_t n, const uint8_t* m) {
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Live LCD mirror (see bandwatch_core.h / docs/DEVELOPER.md section 19). g_mirror is toggled by the "mirror"
// command below; the LVGL flush callback calls mirrorOnFlush() for every flushed region while it is on.
bool g_mirror = false;
static uint16_t mirFrameSeq = 0;     // MF sequence number (wraps; the host only uses it to spot gaps)
static bool mirFrameDrop = false;    // a region of the refresh in progress was dropped

void mirrorOnFlush(int x1, int y1, int x2, int y2, const uint8_t* px, bool last) {
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w > 0 && h > 0) {
        // Sent in row slices of <= ~2 KB base64, each its own whole "M" line (the drop-whole-lines rule, section 6).
        // A flush chunk is up to ~15 full-width rows (~7 KB base64): sent as one line it only fit a nearly empty
        // 8 KB TX buffer, so on busy pages most chunks dropped and the repair never caught up (no complete
        // frame ever). Slices go out while there is room; only the rows that did not fit join the repair.
        const int rowBytes = w * 2;                     // RGB565, little-endian (LV_COLOR_16_SWAP is 0)
        int sliceRows = 1536 / rowBytes;                // ~2 KB once base64-encoded
        if (sliceRows < 1) sliceRows = 1;
        for (int y = y1; y <= y2; y += sliceRows) {
            const int rows = (y + sliceRows - 1 <= y2) ? sliceRows : (y2 - y + 1);
            const int nbytes = rows * rowBytes;
            if (!serialRoom(nbytes * 4 / 3 + 48)) {     // no room for this slice: the rest of the region is repair
                mirFrameDrop = true;
                mirrorNoteDrop(x1, y, x2, y2);
                break;
            }
            Serial.printf("M %d %d %d %d ", x1, y, w, rows);
            writeBase64(px + static_cast<size_t>(y - y1) * rowBytes, static_cast<size_t>(nbytes));
            Serial.write('\n');
        }
    }
    if (!last) return;
    // End of one LVGL refresh = a frame boundary. complete=1 means every region of this refresh went out AND no
    // repair is outstanding, i.e. the host's back buffer now equals the panel: a tear-free frame to publish.
    const bool repairPending = mirrorFrameEnd();
    if (serialRoom(24))
        Serial.printf("MF %u %d\n", static_cast<unsigned>(mirFrameSeq), (mirFrameDrop || repairPending) ? 0 : 1);
    mirFrameSeq++;
    mirFrameDrop = false;
}

namespace {

// The whole argument must be the address: %n records where the match ended and anything after it is rejected.
// Without that, an 8-byte 802.15.4 id "aa:..:ff:00:11" parsed here as a 6-byte MAC and the key hunt never ran.
bool parseMac(const char* s, uint8_t* out) {
    unsigned v[6];
    int end = 0;
    const bool colon = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) == 6 && !s[end];
    if (!colon) {
        end = 0;
        if (sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) != 6 || s[end]) return false;
    }
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

void fmtKey154(char* out, size_t n, const Dev154& d) {
    if (d.flags & 1) snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5], d.key[6], d.key[7]);
    else snprintf(out, n, "%04x/%04x", d.pan, d.shortAddr);
}

} // namespace

// Write a JSON string literal (quoted, escaped) to Serial: exactly jsonStrLen(s) bytes.
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

namespace {

// C4 event-log state, as one JSON object body (~150 B): armed, card usable, baseline in RAM / in the file, rows
// written / waiting, surveillance and new-device counts, rows dropped, card errors, novelty checks skipped for
// want of a baseline.
void printEvents() {
    const EventStats& e = g_evStats;
    Serial.printf("\"ev\":{\"on\":%d,\"card\":%d,\"base\":%d,\"file\":%lu,\"written\":%lu,\"pending\":%lu,"
                  "\"surv\":%lu,\"new\":%lu,\"drop\":%lu,\"err\":%lu,\"wait\":%lu}",
                  g_eventsOn ? 1 : 0, e.cardOk ? 1 : 0, eventsBaseCount(), static_cast<unsigned long>(e.baseFile),
                  static_cast<unsigned long>(e.written), static_cast<unsigned long>(e.pending),
                  static_cast<unsigned long>(e.surv), static_cast<unsigned long>(e.fresh),
                  static_cast<unsigned long>(e.dropped + e.qDropped), static_cast<unsigned long>(e.errors),
                  static_cast<unsigned long>(e.waiting));
}

void printHunt() {
    if (!hunt.active) { Serial.print("\"h\":null"); return; }
    const uint32_t last = hunt.lastMs;
    Serial.printf("\"h\":[%d,%lu,%lu]", hunt.rssi, static_cast<unsigned long>(last ? millis() - last : 0xFFFFFFFFul),
                  static_cast<unsigned long>(hunt.count));
}

// [bssid, park channel (0 if hopping), frames sent, frames failed]
void printDeauth() {
    if (!deauth.active) { Serial.print("\"deauth\":null"); return; }
    const bool targeted = deauth.targeted;   // read once: the attack can stop between the two uses below
    char mac[26];
    fmtMac(mac, sizeof(mac), targeted ? deauth.targetMac : deauth.bssid);
    if (targeted) {
        char ap[26];
        fmtMac(ap, sizeof(ap), deauth.bssid);
        Serial.printf("\"deauth\":[\"%s\",\"%s\",1,%lu,%lu]", mac, ap,
                      static_cast<unsigned long>(deauth.sent), static_cast<unsigned long>(deauth.txFail));
    } else {
        Serial.printf("\"deauth\":[\"%s\",%d,%lu,%lu]", mac, parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                      static_cast<unsigned long>(deauth.sent), static_cast<unsigned long>(deauth.txFail));
    }
}

} // namespace

void huntIdText(char* out, size_t n) {
    if (hunt.kind == 1) {
        if (hunt.key[0] == 0xFF && hunt.key[1] == 0xFE)
            snprintf(out, n, "%04x/%04x", hunt.key[2] | (hunt.key[3] << 8), hunt.key[4] | (hunt.key[5] << 8));
        else
            snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", hunt.key[0], hunt.key[1], hunt.key[2], hunt.key[3],
                     hunt.key[4], hunt.key[5], hunt.key[6], hunt.key[7]);
    } else if (hunt.kind == 2) {
        snprintf(out, n, "%s", hunt.label);   // the LCD's "hunt <id>" line; the protocol escapes it separately
    } else {
        fmtMac(out, n, hunt.mac);
    }
}

// C5 patrol state as one JSON member: "pt":null, or "pt":{"leg":i,"left":ms,"cyc":n,"legs":[["spec",30],...]}.
// Widest case (6 legs of a 4-char mode at 600 s, left 600000, cyc 65535) is 127 B = kPatrolJsonMax - 1.
size_t fmtPatrol(char* out, size_t n) {
    if (!patrol.active) return static_cast<size_t>(snprintf(out, n, "\"pt\":null"));
    int w = snprintf(out, n, "\"pt\":{\"leg\":%u,\"left\":%lu,\"cyc\":%u,\"legs\":[", patrol.leg,
                     static_cast<unsigned long>(patrolLeftMs()), patrol.cycles);
    for (uint8_t i = 0; i < patrol.n && w > 0 && static_cast<size_t>(w) < n; i++)
        w += snprintf(out + w, n - w, "%s[\"%s\",%u]", i ? "," : "", kBandName[patrol.mode[i]], patrol.sec[i]);
    if (w > 0 && static_cast<size_t>(w) < n) w += snprintf(out + w, n - w, "]}");
    return w > 0 ? static_cast<size_t>(w) : 0;
}

namespace {

// The "hunt" id as a JSON value for the hello line: null, a MAC / 15.4 id string, or for an SSID hunt (C7) the
// escaped name followed by ,"ssid":<name> - an old host keeps working (it shows the name as the target, with
// the "h" readings), a new one sees "ssid" and knows it is a network name. Bytes it writes: huntJsonLen().
size_t huntJsonLen() {
    if (!hunt.active) return 4;
    if (hunt.kind == 2) return 2 * jsonStrLen(hunt.label) + 8;   // <name>,"ssid":<name>
    return 25;                                                    // a quoted 23-char 15.4 id at most
}

void printHuntJson() {
    if (!hunt.active) { Serial.print("null"); return; }
    if (hunt.kind == 2) {
        printJsonStr(hunt.label);
        Serial.print(",\"ssid\":");
        printJsonStr(hunt.label);
        return;
    }
    char id[26];
    huntIdText(id, sizeof(id));
    Serial.printf("\"%s\"", id);
}

} // namespace

void sendHello() {
    // Worst case, from the format strings below with every field at its widest: 132 B of literal text in the
    // first printf + ver/dwell/step/band (16) + the channel list (kChannelCount x 4, "165,") + the park..hunt
    // printf (119, incl. a 23-char 15.4 hunt id) + "h" (32) + a targeted "deauth" (74) + "sd" (166, incl. a 47-char
    // path) + mir/alerts (20) + "ev" (189) + "}\n" = 750 + 152 = 902, + C5's ",\"pt\":{...}" (128, kPatrolJsonMax)
    // = 1,030, plus the four esp_err_to_name() strings,
    // measured at run time because their length is the one part not bounded here. A line that passes the check
    // and then overruns is truncated mid-JSON, which is exactly what the drop-whole-lines rule exists to prevent.
    // An SSID hunt (C7) replaces the 25-byte hunt id with the escaped name twice ("hunt" and "ssid"): measured too.
    constexpr size_t kHelloFixed = 750 + kPatrolJsonMax + kChannelCount * 4;
    const size_t errLen = strlen(esp_err_to_name(errCountry)) + strlen(esp_err_to_name(errBand)) +
                          strlen(esp_err_to_name(errProto)) + strlen(esp_err_to_name(errPromisc));
    const size_t huntLen = huntJsonLen();
    if (!serialRoom(kHelloFixed + errLen + (huntLen > 25 ? huntLen - 25 : 0))) return;
    Serial.printf("{\"t\":\"hello\",\"fw\":\"bandwatch\",\"ver\":\"%s\",\"dwell_ms\":%u,\"spec_step\":%u,\"band\":\"%s\",\"country\":\"%s\",\"bandmode\":\"%s\","
                  "\"proto\":\"%s\",\"promisc\":\"%s\",\"chs\":[",
                  kVersion, static_cast<unsigned>(dwellMs()), static_cast<unsigned>(specStepMhz), kBandName[bandMode], esp_err_to_name(errCountry), esp_err_to_name(errBand),
                  esp_err_to_name(errProto), esp_err_to_name(errPromisc));
    bool first = true;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        Serial.printf("%s%u", first ? "" : ",", kChannels[i]);
        first = false;
    }
    static const char* const kRst[] = {"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
                                       "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"};
    const int rr = static_cast<int>(esp_reset_reason());
    Serial.printf("],\"park\":%d,\"cap\":%d,\"snap\":%u,\"heap\":%u,\"up\":%lu,\"rst\":\"%s\",\"hunt\":",
                  parkedIdx >= 0 ? kChannels[parkedIdx] : 0, captureEnabled ? 1 : 0,
                  static_cast<unsigned>(capSnapLen), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(millis() / 1000), (rr >= 0 && rr < 16) ? kRst[rr] : "?");
    printHuntJson();
    Serial.print(",");
    printHunt();
    Serial.print(",");
    printDeauth();
    Serial.printf(",\"sd\":{\"mounted\":%d,\"mb\":%lu,\"cap\":%d,\"file\":\"%s\",\"frames\":%lu,\"bytes\":%lu,\"err\":%lu,\"clock\":%d}",
                  sd.cardPresent ? 1 : 0, static_cast<unsigned long>(sd.cardMb),
                  sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "",
                  static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes),
                  static_cast<unsigned long>(sd.errors), epochValid ? 1 : 0);
    Serial.printf(",\"mir\":%d,\"alerts\":%d,", g_mirror ? 1 : 0, g_ledAlerts ? 1 : 0);
    printEvents();
    char pt[kPatrolJsonMax + 8];
    fmtPatrol(pt, sizeof(pt));
    Serial.printf(",%s}\n", pt);
}

void sendDwell(int idx) {
    if (modeSpec()) {
        // Fine spectrum: lightweight per-dwell line so the dashboard can walk the current frequency live.
        if (!serialRoom(120) || idx < 0 || idx >= specBinCount()) return;
        const SpecBin& b = specFine[idx];
        Serial.printf("{\"t\":\"fd\",\"mhz\":%d,\"min\":%d,\"mean\":%d,\"max\":%d,\"ns\":%u,\"step\":%u,\"n\":%lu}\n",
                      specBinMhz(idx), b.edMin, b.edMean, b.edMax, b.edSamples, specStepMhz,
                      static_cast<unsigned long>(sweepCount));
        return;
    }
    if (!serialRoom(420)) return;   // +~36 B vs 380 for C6's "top"/"trssi" fields (see preamble serial budget)
    const ChannelState& ch = channels[idx];
    // C6 top talker: g_accum still holds this dwell's loudest transmitter (resetAccum() runs after sendDwell).
    uint8_t topMac[6]; int8_t topRssi;
    portENTER_CRITICAL(&g_accumMux);
    topRssi = g_accum.bestRssi;
    for (int i = 0; i < 6; i++) topMac[i] = g_accum.bestMac[i];
    portEXIT_CRITICAL(&g_accumMux);
    char topField[48];
    if (topRssi > -128) {
        char tm[26];
        fmtMac(tm, sizeof(tm), topMac);
        snprintf(topField, sizeof(topField), "\"top\":\"%s\",\"trssi\":%d", tm, topRssi);
    } else {
        snprintf(topField, sizeof(topField), "\"top\":null,\"trssi\":null");   // no attributable frame this dwell
    }
    Serial.printf("{\"t\":\"d\",\"c\":%u,\"s\":%.1f,\"r\":%.1f,\"f\":%lu,\"b\":%lu,\"st\":%u,\"u\":%u,"
                  "\"g\":%.1f,\"n\":%lu,\"park\":%d,\"cap\":%d,\"drop\":%lu,\"da\":%lu,\"df\":%lu,"
                  "\"sdc\":%d,\"sdf\":%lu,\"sdb\":%lu,%s,",
                  kChannels[idx], ch.busyEma, ch.busyCurrent,
                  static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                  ch.metrics.strong, ch.metrics.unique, globalActivityMax(),
                  static_cast<unsigned long>(sweepCount), parkedIdx >= 0 ? kChannels[parkedIdx] : 0,
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped), static_cast<unsigned long>(deauth.sent),
                  static_cast<unsigned long>(deauth.txFail),
                  sd.capEnabled ? 1 : 0, static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes),
                  topField);
    printHunt();
    Serial.print("}\n");
}

void sendSweep() {
    if (modeSpec()) {
        // Fine spectrum: one message per full sweep, all bins, as [edMin, edMean, edMax, edSamples] in order.
        // Frequency is lo + i*step (not sent per bin). Up to 84 bins (~2 kB) — dropped whole if no room.
        // Budget: 84 B of header + tail with the counters at their widest, 23 B per bin (",[-128,-128,-128,65535]").
        const int nb = specBinCount();
        if (!serialRoom(static_cast<size_t>(nb) * 23 + 84)) return;
        Serial.printf("{\"t\":\"fs\",\"n\":%lu,\"step\":%u,\"lo\":%d,\"count\":%d,\"bins\":[",
                      static_cast<unsigned long>(sweepCount), specStepMhz, kSpecLoMhz, nb);
        for (int i = 0; i < nb; i++) {
            const SpecBin& b = specFine[i];
            Serial.printf("%s[%d,%d,%d,%u]", i ? "," : "", b.edMin, b.edMean, b.edMax, b.edSamples);
        }
        Serial.printf("],\"heap\":%u}\n", static_cast<unsigned>(ESP.getFreeHeap()));
        return;
    }
    // Budget: 54 B of prefix and 51 B of tail with the counters at their widest, plus 48 B per channel
    // (",[165,100.0,<frames 10>,<bytes 10>,65535,65535,2]"). "both" (38 channels) comes to 1,929 B; the old flat
    // 1500 could pass the check and then overrun on a busy channel set.
    if (!serialRoom(105 + static_cast<size_t>(enabledCount()) * 48)) return;
    Serial.printf("{\"t\":\"s\",\"n\":%lu,\"g\":%.1f,\"band\":\"%s\",\"ch\":[", static_cast<unsigned long>(sweepCount),
                  globalActivityMax(), kBandName[bandMode]);
    bool first = true;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        const int state = ch.unavailable ? 2 : (ch.hasData ? 0 : 1);
        Serial.printf("%s[%u,%.1f,%lu,%lu,%u,%u,%d]", first ? "" : ",", kChannels[i], ch.busyEma,
                      static_cast<unsigned long>(ch.metrics.frames), static_cast<unsigned long>(ch.metrics.bytes),
                      ch.metrics.strong, ch.metrics.unique, state);
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
    for (int i = 0; i < kBleDevSlots; i++) if (devFresh(bleDevs[i].lastMs, now, kDevFreshMs)) n++;
    portEXIT_CRITICAL(&g_devMux);
    Serial.printf("{\"t\":\"ble\",\"devs\":%d,\"cycles\":%lu,\"heap\":%u,\"adv\":%lu,\"scan\":\"%s\",\"running\":\"%s\",\"switches\":%lu,"
                  "\"cap\":%d,\"drop\":%lu,\"sdc\":%d,\"sdf\":%lu,\"sdb\":%lu,",
                  n, static_cast<unsigned long>(bleScan.cycles), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(bleScan.advSeen), bleScanModeName(),
                  bleScan.active ? "active" : "passive", static_cast<unsigned long>(bleScan.switches),
                  captureEnabled ? 1 : 0, static_cast<unsigned long>(capDropped),
                  sd.capEnabled ? 1 : 0, static_cast<unsigned long>(sd.frames),
                  static_cast<unsigned long>(sd.bytes));
    printHunt();
    Serial.print("}\n");
}

// Device tables -> host. Wi-Fi: [mac, rssi, max, frames, age_ms, ch, flags, ssid]; BLE: [mac, rssi, max, adv, age_ms, addrType, company, name]
void sendDevices() {
    const uint32_t now = millis();
    char mac[24];
    DevRef* refs = g_devRefs;
    if (mode154()) {
        const int n = collect154Refs(refs, kDev154Slots, kDevFreshMs);
        if (!serialRoom(40 + n * 84)) return;   // a row at its widest (extended id, every number maxed) is 83 B
        // [id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]
        Serial.print("{\"t\":\"z\",\"dev\":[");
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            Dev154 d;
            if (!fetch154Dev(refs[i], d, kDevFreshMs)) continue;   // slot reused/evicted since collect: skip
            fmtKey154(mac, sizeof(mac), d);
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,%u,%u,%u,%u]", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                          static_cast<unsigned long>(ageMs(now, d.lastMs)), d.ch, d.pan, d.shortAddr, d.proto, d.flags, d.lqi);
        }
        Serial.print("]}\n");
        return;
    }
    if (wifiMode()) {
        const int n = collectWifiRefs(refs, kWifiDevSlots, kDevFreshMs);
        // Sent in chunks of kWifiRowsPerLine rows, loudest first: a full 96-slot table (~12 kB) cannot fit the 8 KB
        // TX buffer as one line. Each chunk is budgeted on its own, and the host merges rows by MAC, so a chunk that
        // does not fit this cycle (the quietest devices, last in RSSI order) just waits for the next one. Every
        // chunk is a complete "w" line.
        //
        // Budget per row = kWifiRowFixed (every number at its widest, the "aabbcc" suffix and the punctuation) + the
        // real escaped length of ssid and cc: off-air strings keep '"' and '\\', which escape to 2 bytes, so a
        // 32-char SSID can take 66. A first pass sums that over the chunk; the emitting pass re-fetches each row and
        // skips (whole) any row that has grown past what is left, e.g. a beacon re-parsed in between.
        constexpr int kWifiRowsPerLine = 24;
        constexpr size_t kWifiRowFixed = 98;
        for (int base = 0; base < n || (base == 0 && n == 0); base += kWifiRowsPerLine) {
            const int end = LV_MIN(n, base + kWifiRowsPerLine);
            size_t budget = 0;
            for (int i = base; i < end; i++) {
                WifiDev d;
                if (fetchWifiDev(refs[i], d, kDevFreshMs))
                    budget += kWifiRowFixed + jsonStrLen(d.ssid) + jsonStrLen(d.cc[0] ? d.cc : "");
            }
            if (!serialRoom(40 + budget)) return;
            Serial.print("{\"t\":\"w\",\"dev\":[");
            int emitted = 0;
            for (int i = base; i < end; i++) {
                WifiDev d;
                if (!fetchWifiDev(refs[i], d, kDevFreshMs)) continue;
                const size_t rowLen = kWifiRowFixed + jsonStrLen(d.ssid) + jsonStrLen(d.cc[0] ? d.cc : "");
                if (rowLen > budget) continue;
                budget -= rowLen;
                fmtMac(mac, sizeof(mac), d.mac);
                Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                              static_cast<unsigned long>(ageMs(now, d.lastMs)), d.ch, d.flags);
                printJsonStr(d.ssid);
                Serial.printf(",%u,%u,%u,%u,%u,%u,", d.sec, d.pmf, d.phy, d.bw, d.util, d.stations);
                printJsonStr(d.cc[0] ? d.cc : "");   // country IE is 2 raw bytes off the air: escape it like every other string
                // Association suffix as "aabbcc", or "" when this device was never seen on a BSS. The host joins
                // it against the APs it already knows to recover the full BSSID (see docs/DEVELOPER.md §17).
                if (d.apSuffix[0] || d.apSuffix[1] || d.apSuffix[2])
                    Serial.printf(",%u,\"%02x%02x%02x\"]", d.surv, d.apSuffix[0], d.apSuffix[1], d.apSuffix[2]);
                else
                    Serial.printf(",%u,\"\"]", d.surv);
            }
            Serial.print("]}\n");
            if (n == 0) break;
        }
    } else {
        const int n = collectBleRefs(refs, kBleDevSlots, kDevFreshMs);
        // Budgeted like the Wi-Fi chunks: kBleRowFixed is a row with every number at its widest (int8 -128s,
        // uint16 65535s, a 10-digit age) and the punctuation, plus the real escaped length of the name (a
        // 20-char name of '"' is 42 B). An under-budget row would truncate the line mid-write instead of
        // dropping it whole. Worst case 48 x (95 + 42) + 40 = 6.6 kB, under the 8 KB TX buffer.
        constexpr size_t kBleRowFixed = 95;
        size_t budget = 0;
        for (int i = 0; i < n; i++) {
            BleDev d;
            if (fetchBleDev(refs[i], d, kDevFreshMs)) budget += kBleRowFixed + jsonStrLen(d.name);
        }
        if (!serialRoom(40 + budget)) return;
        Serial.print("{\"t\":\"b\",\"dev\":[");
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            BleDev d;
            if (!fetchBleDev(refs[i], d, kDevFreshMs)) continue;
            const size_t rowLen = kBleRowFixed + jsonStrLen(d.name);
            if (rowLen > budget) continue;   // renamed since the first pass and no longer fits: skip it whole
            budget -= rowLen;
            fmtMac(mac, sizeof(mac), d.mac);
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.adv,
                          static_cast<unsigned long>(ageMs(now, d.lastMs)), d.addrType, d.company);
            printJsonStr(d.name);
            Serial.printf(",%u,%d,%u,%u,%u,%u,%u]", d.appearance, d.txPower, d.svc, d.svcData, d.appleType,
                          d.flags, d.surv);
        }
        Serial.print("]}\n");
    }
}

namespace {

// Parse "aa:bb:cc:dd:ee:ff:00:11" (extended address) or "pan/short" (hex) into an 802.15.4 key.
// Like parseMac, the whole argument must match (%n + end check).
bool parseKey154(const char* s, uint8_t* key) {
    unsigned v[8];
    int end = 0;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &end) == 8 && !s[end]) {
        for (int i = 0; i < 8; i++) key[i] = static_cast<uint8_t>(v[i]);
        return true;
    }
    unsigned pan, sh;
    end = 0;
    if (sscanf(s, "%4x/%4x%n", &pan, &sh, &end) == 2 && !s[end]) {
        key[0] = 0xFF; key[1] = 0xFE; key[2] = pan & 0xFF; key[3] = (pan >> 8) & 0xFF;
        key[4] = sh & 0xFF; key[5] = (sh >> 8) & 0xFF; key[6] = 0; key[7] = 0;
        return true;
    }
    return false;
}

// C5: "spec:30,both:40,ble:20" -> legs. The whole argument must parse: kPatrolMinLegs..kPatrolMaxLegs legs of
// <mode>:<seconds>, mode by its kBandName, seconds kPatrolSecMin..kPatrolSecMax as plain digits.
bool parsePatrolLegs(char* s, BandMode* modes, uint16_t* secs, uint8_t& n) {
    n = 0;
    for (;;) {
        if (n >= kPatrolMaxLegs) return false;
        char* colon = strchr(s, ':');
        if (!colon) return false;
        *colon = 0;
        int m = -1;
        for (int i = 0; i < kBandModes; i++) if (!strcmp(s, kBandName[i])) m = i;
        const char* p = colon + 1;
        unsigned sec = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) { sec = sec * 10 + static_cast<unsigned>(*p++ - '0'); digits++; }
        if (m < 0 || !digits || sec < kPatrolSecMin || sec > kPatrolSecMax) return false;
        modes[n] = static_cast<BandMode>(m);
        secs[n] = static_cast<uint16_t>(sec);
        n++;
        if (!*p) break;
        if (*p != ',' || !p[1]) return false;
        s = const_cast<char*>(p + 1);
    }
    return n >= kPatrolMinLegs;
}

// The hunt/huntssid ack: {"t":"ack","cmd":"hunt","hunt":<id|name|null>[,"ssid":<name>],"park":N}. Escaped with
// jsonQuote and sent whole through sendLinef (a 32-byte name of '"' escapes to 66 bytes, twice: inside its 320).
void sendHuntAck() {
    const int park = parkedIdx >= 0 ? kChannels[parkedIdx] : 0;
    if (!hunt.active) {
        sendLinef("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":null,\"park\":%d}\n", park);
    } else if (hunt.kind == 2) {
        char q[2 * 32 + 3];
        if (jsonQuote(q, sizeof(q), hunt.label))
            sendLinef("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":%s,\"ssid\":%s,\"park\":%d}\n", q, q, park);
    } else {
        char m[26];
        huntIdText(m, sizeof(m));
        sendLinef("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":\"%s\",\"park\":%d}\n", m, park);
    }
}

void handleCommand(char* line) {
    // One command per line; the full list and each reply's shape are in docs/DEVELOPER.md (and CLAUDE.md).
    // Every ack/err/log reply goes through sendLinef(), so a full TX buffer drops the reply whole (rule 6).
    char* sp = strchr(line, ' ');
    char* arg = const_cast<char*>("");
    if (sp) { *sp = 0; arg = sp + 1; }
    if (patrol.active) {
        // C5 v1 patrols without capture (every hand-off releases the ring), and a hunt or deauth parks the radio,
        // which a hand-off would undo: none of them may start mid-patrol (patrol refuses to start over them too).
        // The stop forms ("cap 0", "hunt 0", "deauth 0", "dca 0") still pass. C7's "huntssid" is matched by its
        // command word only, so this guard needs nothing from that code.
        const bool capStart = (!strcmp(line, "cap") || !strcmp(line, "sdcap")) && atoi(arg) != 0;
        const bool parkStart = (!strcmp(line, "hunt") || !strcmp(line, "huntssid") || !strcmp(line, "deauth") ||
                                !strcmp(line, "dca")) && arg[0] && strcmp(arg, "0") != 0;
        if (capStart || parkStart) {
            sendLinef("{\"t\":\"err\",\"msg\":\"%s: stop patrol first\"}\n", line);
            return;
        }
    }
    if (!strcmp(line, "patrol")) {
        // "patrol 1" = the default legs, "patrol <mode>:<sec>,..." = custom legs (both start, or restart, it),
        // "patrol 0" = stop and stay in the current mode, bare "patrol" = query. Never persisted.
        if (!strcmp(arg, "0")) {
            stopPatrol();
        } else if (arg[0]) {
            static const BandMode kDefModes[] = {BAND_SPEC, BAND_BOTH, BAND_BLE};
            static const uint16_t kDefSecs[] = {30, 40, 20};
            BandMode modes[kPatrolMaxLegs];
            uint16_t secs[kPatrolMaxLegs];
            uint8_t n = 0;
            const char* why = nullptr;
            if (!strcmp(arg, "1")) {
                for (n = 0; n < 3; n++) { modes[n] = kDefModes[n]; secs[n] = kDefSecs[n]; }
            } else if (!parsePatrolLegs(arg, modes, secs, n)) {
                why = "legs are 2-6 x mode:sec (5g|2.4g|both|ble|154|spec, 5-600 s)";
            }
            if (!why && (captureEnabled || sd.capEnabled)) why = "stop capture first";
            else if (!why && hunt.active) why = "stop hunt first";
            else if (!why && deauth.active) why = "stop deauth first";
            if (why) {
                sendLinef("{\"t\":\"err\",\"msg\":\"patrol: %s\"}\n", why);
                return;
            }
            startPatrol(modes, secs, n);
        }
        char pt[kPatrolJsonMax + 8];
        fmtPatrol(pt, sizeof(pt));
        sendLinef("{\"t\":\"ack\",\"cmd\":\"patrol\",%s}\n", pt);
    } else if (!strcmp(line, "cap")) {
        // Wi-Fi / 802.15.4 / BLE all have a link type; spec arms no RX, so refuse rather than record silence.
        const bool want = atoi(arg) != 0;
        const bool on = want && !modeSpec() && ensureCapRing();
        captureEnabled = on;
        syncCapActive();
        if (want && modeSpec()) sendLinef("{\"t\":\"log\",\"msg\":\"cap: no RX armed in spec mode\"}\n");
        sendLinef("{\"t\":\"ack\",\"cmd\":\"cap\",\"cap\":%d}\n", captureEnabled ? 1 : 0);
    } else if (!strcmp(line, "sdcap")) {
        const bool want = atoi(arg) != 0;
        if (want && modeSpec() && !sd.capEnabled) {   // no RX armed in spec: the file would never grow
            sendLinef("{\"t\":\"err\",\"msg\":\"sdcap: no RX armed in spec mode\"}\n");
        } else if (want && !sd.capEnabled) {
            // Open the file first: mounting FATFS costs ~30 KB, and the ring must be sized against what is
            // left afterwards or kCapHeapReserve is not actually reserved. A ring that "cap 1" already made
            // was sized before the mount, so refit re-sizes it if the mount pushed heap under the floor.
            if (!sdOpenCapture()) {
                sendLinef("{\"t\":\"err\",\"msg\":\"sdcap: %s\"}\n",
                          sd.mounted ? "could not open file on card" : "no SD card (check it is inserted)");
            } else if (!refitCapRing()) {
                sendLinef("{\"t\":\"err\",\"msg\":\"sdcap: no capture ring\"}\n");
                sdCloseCapture();
            } else if (serialRoom(140)) {
                sendLinef("{\"t\":\"log\",\"msg\":\"sd capture -> %s\"}\n", sd.path);
            }
        } else if (!want) {
            sdCloseCapture();
        }
        syncCapActive();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"sdcap\",\"sdcap\":%d,\"file\":\"%s\"}\n",
                  sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "");
    } else if (!strcmp(line, "sdinfo")) {
        const bool m = sdMount();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"sdinfo\",\"sd\":%d,\"mb\":%lu,\"used_mb\":%lu,\"cap\":%d,\"file\":\"%s\","
                  "\"frames\":%lu,\"bytes\":%lu,\"err\":%lu}\n",
                  m ? 1 : 0, m ? static_cast<unsigned long>(SD.cardSize() / (1024 * 1024)) : 0UL,
                  m ? static_cast<unsigned long>(SD.usedBytes() / (1024 * 1024)) : 0UL,
                  sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "",
                  static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes),
                  static_cast<unsigned long>(sd.errors));
        sdUnmount();   // sdinfo is a probe, not a mount: it used to leave FATFS (~30 KB) mounted until the next
                       // capture or sdread came along. sdUnmount() keeps it when a capture/sdread owns the card.
    } else if (!strcmp(line, "sdface")) {    // diagnostic: show the card-out (0) / card-in (1) face without touching the card
        showSdFace(atoi(arg) != 0, "test");
        sendLinef("{\"t\":\"ack\",\"cmd\":\"sdface\"}\n");
    } else if (!strcmp(line, "sdprobe")) {   // diagnostic: raw CMD0 reply of the presence probe (idle card only)
        const bool idle = !sd.mounted && !sd.capEnabled && !sd.readActive;
        sendLinef("{\"t\":\"ack\",\"cmd\":\"sdprobe\",\"r1\":%d,\"present\":%d}\n",
                  idle ? sdProbeR1() : -1, sd.cardPresent ? 1 : 0);
    } else if (!strcmp(line, "sdls")) {
        sdListFiles();
    } else if (!strcmp(line, "sdread")) {
        sdReadFile(arg);
    } else if (!strcmp(line, "sdrm")) {
        sdRemoveFile(arg);   // acks {"cmd":"sdrm","file","ok"}; refusals are "sdrm: ..." err lines
    } else if (!strcmp(line, "events")) {
        // C4: arm/disarm the SD event log (persisted). Arming works with no card: it retries the mount.
        if (atoi(arg) != 0) { if (!eventsEnable()) sendLinef("{\"t\":\"err\",\"msg\":\"events: not enough free heap\"}\n"); }
        else eventsDisable();
        saveSettings();
        if (serialRoom(220)) { Serial.print("{\"t\":\"ack\",\"cmd\":\"events\","); printEvents(); Serial.print("}\n"); }   // 217 B at its widest
    } else if (!strcmp(line, "alerts")) {
        // D1: LED alert blips (surveillance / permit-join / new device), persisted. Never affects the deauth blink.
        g_ledAlerts = atoi(arg) != 0;
        saveSettings();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"alerts\",\"alerts\":%d}\n", g_ledAlerts ? 1 : 0);
    } else if (!strcmp(line, "ledtest")) {
        // Diagnostic: draw one blip now (bypasses the rate limit and the alerts switch; not over an active deauth).
        const LedAlertKind k = !strcmp(arg, "surv") ? LED_ALERT_SURV : !strcmp(arg, "join") ? LED_ALERT_JOIN
                             : !strcmp(arg, "new") ? LED_ALERT_NEW : LED_ALERT_NONE;
        if (k == LED_ALERT_NONE) {
            sendLinef("{\"t\":\"err\",\"msg\":\"ledtest: surv|new|join\"}\n");
        } else {
            ledAlertTest(k);
            sendLinef("{\"t\":\"ack\",\"cmd\":\"ledtest\",\"kind\":\"%s\",\"shown\":%d}\n", arg, ledBlipActive() ? 1 : 0);
        }
    } else if (!strcmp(line, "addr1")) {
        trackAddr1 = atoi(arg) != 0;
        saveSettings();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"addr1\",\"addr1\":%d}\n", trackAddr1 ? 1 : 0);
    } else if (!strcmp(line, "blescan")) {
        if (!strcmp(arg, "active"))       bleScan.mode = BLE_SCAN_ACTIVE;
        else if (!strcmp(arg, "passive")) bleScan.mode = BLE_SCAN_PASSIVE;
        else if (!strcmp(arg, "auto"))    bleScan.mode = BLE_SCAN_AUTO;
        bleScan.lastSwitchMs = 0;   // apply the new policy on the next serviceBle() without waiting out the limit
        saveSettings();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"blescan\",\"mode\":\"%s\",\"running\":\"%s\"}\n",
                  bleScanModeName(), bleScan.active ? "active" : "passive");
    } else if (!strcmp(line, "time")) {
        // ok = this value was applied. An out-of-range value leaves the clock as it was and acks ok 0, even
        // when an earlier "time" already set it (the ack used to echo epochValid, which read as "applied").
        const uint32_t e = strtoul(arg, nullptr, 10);
        const bool applied = e > 1600000000UL;
        if (applied) { epochBase = e; epochBaseMs = millis(); epochValid = true; }
        sendLinef("{\"t\":\"ack\",\"cmd\":\"time\",\"epoch\":%lu,\"ok\":%d}\n",
                  static_cast<unsigned long>(e), applied ? 1 : 0);
    } else if (!strcmp(line, "snap")) {
        int n = atoi(arg);
        if (n < 32) n = 32;
        if (n > kCapMaxLen) n = kCapMaxLen;
        capSnapLen = n;
        saveSettings();
        sendLinef("{\"t\":\"ack\",\"cmd\":\"snap\",\"snap\":%d}\n", n);
    } else if (!strcmp(line, "park")) {
        const int ch = atoi(arg);
        const int idx = (ch > 0) ? indexOfChannel(ch) : -1;
        setPark((idx >= 0 && chanEnabled(idx)) ? idx : -1);
        sendLinef("{\"t\":\"ack\",\"cmd\":\"park\",\"park\":%d}\n", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
    } else if (!strcmp(line, "band")) {
        const BandMode prev = bandMode;
        int m = -1;
        if (!strcmp(arg, "5g")) m = BAND_5G;
        else if (!strcmp(arg, "2.4g") || !strcmp(arg, "24g")) m = BAND_24G;
        else if (!strcmp(arg, "both")) m = BAND_BOTH;
        else if (!strcmp(arg, "ble")) m = BAND_BLE;
        else if (!strcmp(arg, "154") || !strcmp(arg, "zigbee") || !strcmp(arg, "thread")) m = BAND_154;
        else if (!strcmp(arg, "spec") || !strcmp(arg, "spectrum")) m = BAND_SPEC;
        const bool wasPatrolling = m >= 0 && patrol.active;
        if (wasPatrolling) {   // a manual mode choice ends the patrol (C5), even onto the leg it was already on
            stopPatrol();
            sendLinef("{\"t\":\"log\",\"msg\":\"patrol stopped by band\"}\n");
        }
        if (m >= 0) setBandMode(static_cast<BandMode>(m));
        showModeChange(prev);
        if (bandMode != prev || wasPatrolling) saveSettings();   // persist the new mode (C3); park is intentionally not saved
        sendLinef("{\"t\":\"ack\",\"cmd\":\"band\",\"band\":\"%s\"}\n", kBandName[bandMode]);
        sendHello();
    } else if (!strcmp(line, "hunt")) {
        uint8_t mac[6];
        char* sp2 = strchr(arg, ' ');
        int ch = 0;
        if (sp2) { *sp2 = 0; ch = atoi(sp2 + 1); }
        uint8_t key[8];
        if (parseMac(arg, mac)) startHunt(mac, ch);
        else if (parseKey154(arg, key)) {
            startHunt154(key);
            // Park only where the 15.4 channel set is what is hopping: in a Wi-Fi mode its index is not a channel.
            if (ch > 0 && mode154()) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); hunt.parked = true; } }
        }
        else stopHunt();
        sendHuntAck();
        if (hunt.active && hunt.kind == 1 && mode154()) {
            // park on the channel the node was last seen on
            portENTER_CRITICAL(&g_devMux);
            int ch = 0;
            for (int i = 0; i < kDev154Slots; i++) if (devs154[i].lastMs && key8Eq(devs154[i].key, hunt.key)) { ch = devs154[i].ch; break; }
            portEXIT_CRITICAL(&g_devMux);
            if (ch && parkedIdx < 0) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); hunt.parked = true; } }
        }
    } else if (!strcmp(line, "huntssid")) {
        // C7: "huntssid <name>" - the name is the rest of the line, spaces included, 1..32 bytes, matched exactly
        // (case-sensitive) against beacon SSIDs as stored, i.e. after the control-character sanitize, which is
        // applied to the name too. "huntssid 0" (or no name) stops the hunt. A name that cannot be an SSID is
        // refused and the running hunt is left alone. pollSerial's 64-byte line (C5) holds "huntssid " + 32 bytes.
        const size_t n = strlen(arg);
        if (n > 32) {
            sendLinef("{\"t\":\"err\",\"msg\":\"huntssid: name longer than 32 bytes\"}\n");
        } else {
            if (n == 0 || !strcmp(arg, "0")) {
                stopHunt();
            } else {
                char name[33];
                memcpy(name, arg, n + 1);
                sanitizeText(name, sizeof(name));   // rule 9: the same transform as the stored SSIDs it is matched to
                startHuntSsid(name);
            }
            sendHuntAck();
        }
    } else if (!strcmp(line, "deauth")) {
        uint8_t mac[6];
        // Wi-Fi modes only: the frames go out on the STA interface. Unparseable arg or wrong mode stops it.
        if (wifiMode() && parseMac(arg, mac)) startDeauth(mac);
        else stopDeauth();
        char m[26];
        fmtMac(m, sizeof(m), deauth.bssid);
        const int ch = parkedIdx >= 0 ? kChannels[parkedIdx] : 0;   // startDeauth parks before this ack, so ch is known
        // "fc" is the FC byte0 now in force on the internal kick path: startDeauth resets the "kickfc"
        // diagnostic override, and echoing it here is the only place the host can see that it happened.
        if (deauth.active)
            sendLinef("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":[\"%s\",%d,0,0],\"park\":%d,\"fc\":\"0x%02x\"}\n",
                      m, ch, ch, kickFc);
        else
            sendLinef("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":null,\"park\":%d}\n", ch);
    } else if (!strcmp(line, "dca")) {
        // "dca <client_mac> <ap_bssid>": kick one station off one AP. Anything unparseable, a missing second
        // argument or a non-Wi-Fi mode stops the attack rather than starting a broadcast one by accident.
        uint8_t client[6], ap[6];
        char* sp2 = strchr(arg, ' ');
        if (sp2) *sp2 = 0;
        if (wifiMode() && sp2 && parseMac(arg, client) && parseMac(sp2 + 1, ap)) startDeauthTargeted(client, ap);
        else stopDeauth();
        char c[26], a[26];
        fmtMac(c, sizeof(c), deauth.targeted ? deauth.targetMac : deauth.bssid);
        fmtMac(a, sizeof(a), deauth.bssid);
        const int ch = parkedIdx >= 0 ? kChannels[parkedIdx] : 0;   // startDeauthTargeted parks before this ack
        if (deauth.active)
            sendLinef("{\"t\":\"ack\",\"cmd\":\"dca\",\"deauth\":[\"%s\",\"%s\",%d,0,0],\"park\":%d,\"fc\":\"0x%02x\"}\n",
                      c, a, deauth.targeted ? 1 : 0, ch, kickFc);
        else
            sendLinef("{\"t\":\"ack\",\"cmd\":\"dca\",\"deauth\":null,\"park\":%d}\n", ch);
    } else if (!strcmp(line, "kickfc")) {
        // DIAGNOSTIC (§11): override the FC byte0 the internal kick path writes. "kickfc 80" sends a
        // beacon down the deauth descriptor path, so an external monitor can tell whether that path
        // radiates at all. "kickfc c0" restores the real deauth subtype, and so does the next "deauth"
        // or "dca" — the override never survives into an attack the operator did not ask it for.
        // 0 is rejected rather than stored (FC 0x00 is an association request, not anything worth
        // sending), which also makes a bare "kickfc" a query; either way the ack below is the value in
        // force, so a refused write is visible rather than silent.
        const uint8_t v = static_cast<uint8_t>(strtoul(arg, nullptr, 16));
        if (v) kickFc = v;
        sendLinef("{\"t\":\"ack\",\"cmd\":\"kickfc\",\"fc\":\"0x%02x\"}\n", kickFc);
    } else if (!strcmp(line, "kickpath")) {
        // 0 (default) = raw esp_wifi_80211_tx, the only path measured to reach the air (§11).
        // 1 = driver-internal slot, which transmits nothing for any subtype; kept for §9 offset work.
        useInternalKick = atoi(arg) != 0;
        sendLinef("{\"t\":\"ack\",\"cmd\":\"kickpath\",\"internal\":%d}\n", useInternalKick ? 1 : 0);
    } else if (!strcmp(line, "txtest")) {
        // 0 = off, 1 = beacon with promiscuous RX still on, 2 = beacon with promiscuous RX turned off.
        // Mode 2 tests whether promiscuous mode is what stops the PHY from transmitting.
        const int mode = atoi(arg);
        txTestActive = mode != 0 && wifiMode();
        txTestSent = txTestFail = 0;
        esp_err_t pr = ESP_OK;
        if (mode == 2)      pr = esp_wifi_set_promiscuous(false);
        else if (mode <= 0) pr = esp_wifi_set_promiscuous(true);
        sendLinef("{\"t\":\"ack\",\"cmd\":\"txtest\",\"txtest\":%d,\"mode\":%d,\"promisc_call\":\"%s\",\"ch\":%u}\n",
                  txTestActive ? 1 : 0, mode, esp_err_to_name(pr), currentChannelNum);
    } else if (!strcmp(line, "txstat")) {
        sendLinef("{\"t\":\"ack\",\"cmd\":\"txstat\",\"sent\":%lu,\"fail\":%lu,\"ch\":%u}\n",
                  static_cast<unsigned long>(txTestSent), static_cast<unsigned long>(txTestFail), currentChannelNum);
    } else if (!strcmp(line, "specstep")) {
        const int st = atoi(arg);
        if (st == 1 || st == 2 || st == 5) {
            specStepMhz = static_cast<uint8_t>(st);
            if (modeSpec()) { resetChannelStats(); currentIdx = -1; monitorReady = advanceChannel(); }
            saveSettings();
        }
        sendLinef("{\"t\":\"ack\",\"cmd\":\"specstep\",\"step\":%u}\n", specStepMhz);
    } else if (!strcmp(line, "mirror")) {
        g_mirror = (atoi(arg) != 0);
        if (g_mirror) mirrorRequestFull();   // push a full frame now (paced across the next loops)
        sendLinef("{\"t\":\"ack\",\"cmd\":\"mirror\",\"mirror\":%d,\"w\":%d,\"h\":%d}\n",
                  g_mirror ? 1 : 0, LCD_WIDTH, LCD_HEIGHT);
    } else if (!strcmp(line, "page")) {
        stepPage(!strcmp(arg, "prev") ? -1 : 1);   // "next"/empty = forward, like a BOOT tap
        sendLinef("{\"t\":\"ack\",\"cmd\":\"page\",\"page\":%d}\n", currentPage);
    } else if (!strcmp(line, "reboot")) {
        sendLinef("{\"t\":\"ack\",\"cmd\":\"reboot\"}\n");
        delay(50);
        ESP.restart();
    } else if (!strcmp(line, "info")) {
        sendHello();
        if (hopMode()) sendSweep();
        sendDevices();
    } else {
        sendLinef("{\"t\":\"err\",\"msg\":\"unknown command\"}\n");
    }
}

} // namespace

void pollSerial() {
    // 64: the longest valid command is a 6-leg "patrol" (60 chars). A longer line used to be cut and run as its
    // prefix - harmless for a MAC, but a cut leg list can still parse ("both:600" -> "both:6") - so it is refused.
    static char line[64];
    static size_t len = 0;
    static bool over = false;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (over) sendLinef("{\"t\":\"err\",\"msg\":\"line too long\"}\n");
            else if (len) { line[len] = 0; handleCommand(line); }
            len = 0;
            over = false;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        } else {
            over = true;
        }
    }
}

// C1: drain the probe queue. A (MAC, SSID) pair is re-announced at most once per kProbeQuietMs: a probing client
// repeats the same request every few seconds on every channel, and the host keeps the history anyway. A line
// that does not fit is left *unrecorded* in the dedup table, so it goes out on the next sighting instead.
namespace {
constexpr int kProbeSeen = 32;
constexpr uint32_t kProbeQuietMs = 60000;
struct ProbeSeen { uint8_t mac[6]; uint16_t ssidHash; uint32_t lastMs; };
ProbeSeen probeSeen[kProbeSeen];
uint16_t ssidHash(const char* s) {
    uint16_t h = 0x811C;
    for (; *s; s++) h = static_cast<uint16_t>((h ^ static_cast<uint8_t>(*s)) * 0x0101 + 0x3B);
    return h;
}
} // namespace

void serviceProbes() {
    const uint32_t now = millis();
    while (probeTail != probeHead) {
        const ProbeEvt e = g_probeQ[probeTail];   // copy, then release the slot to the producer
        probeTail = static_cast<uint8_t>((probeTail + 1) % kProbeQ);
        const uint16_t h = ssidHash(e.ssid);
        int hit = -1, oldest = 0;
        for (int i = 0; i < kProbeSeen; i++) {
            if (probeSeen[i].lastMs && probeSeen[i].ssidHash == h && macEq(probeSeen[i].mac, e.mac)) { hit = i; break; }
            if (probeSeen[i].lastMs < probeSeen[oldest].lastMs) oldest = i;   // empty slots (0) win
        }
        if (hit >= 0 && now - probeSeen[hit].lastMs < kProbeQuietMs) continue;
        if (!serialRoom(140)) continue;   // 32-char SSID escaped is <= 66 B; the rest is ~60 B
        char mac[18];
        fmtMac(mac, sizeof(mac), e.mac);
        Serial.printf("{\"t\":\"pr\",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u,\"ssid\":", mac, e.rssi, e.ch);
        printJsonStr(e.ssid);
        Serial.print("}\n");
        ProbeSeen& slot = probeSeen[hit >= 0 ? hit : oldest];
        memcpy(slot.mac, e.mac, 6);
        slot.ssidHash = h;
        slot.lastMs = now ? now : 1;
    }
}

// C4: a small status line every 5 s while the event log is armed, so the dashboard's counters move in every mode.
void sendEventStatus() {
    static uint32_t lastMs = 0;
    if (!g_eventsOn) return;
    const uint32_t now = millis();
    if (now - lastMs < 5000 || !serialRoom(210)) return;   // 201 B with every counter at its widest
    lastMs = now;
    Serial.print("{\"t\":\"ev\",");
    printEvents();
    Serial.print("}\n");
}
