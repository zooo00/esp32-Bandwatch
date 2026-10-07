// Host protocol for Bandwatch: one JSON object per line over USB serial; captured frames stream as
// "P ..." base64 lines (the host merges them into a pcap). Line shapes must stay in sync with
// handle_line/merge_* in host/bandwatch_host.py and docs/DEVELOPER.md. Every send checks serialRoom()
// first so a slow or absent host drops whole lines, never half of one.
#include "bandwatch_core.h"
#include "LVGL_Driver.h"   // LCD_WIDTH/LCD_HEIGHT for the mirror ack
#include <WiFi.h>
#include <esp_wifi.h>   // C API for the txtest branch (promiscuous on/off, channel)
#include <SD.h>     // the sdinfo branch reports card size while mounted
#include <string.h>

void fmtMac(char* out, size_t n, const uint8_t* m) {
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Live LCD mirror (see bandwatch_core.h / docs/DEVELOPER.md section 19). g_mirror is toggled by the "mirror"
// command below; the LVGL flush callback calls mirrorOnFlush() for every flushed region while it is on.
bool g_mirror = false;

void mirrorOnFlush(int x1, int y1, int x2, int y2, const uint8_t* px) {
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w <= 0 || h <= 0) return;
    const int nbytes = w * h * 2;   // RGB565, little-endian (LV_COLOR_16_SWAP is 0)
    // base64 is 4/3 the raw size. Keep whole lines - a region is sent intact or not at all (the drop-whole-
    // lines rule, section 6): if the TX buffer can't hold it, skip it and schedule a full re-send so the host
    // still converges once the buffer drains.
    if (!serialRoom(nbytes * 4 / 3 + 48)) { mirrorNoteDrop(); return; }   // re-sent on the next full pass
    Serial.printf("M %d %d %d %d ", x1, y1, w, h);
    writeBase64(px, static_cast<size_t>(nbytes));
    Serial.write('\n');
}

namespace {

bool parseMac(const char* s, uint8_t* out) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

void fmtKey154(char* out, size_t n, const Dev154& d) {
    if (d.flags & 1) snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5], d.key[6], d.key[7]);
    else snprintf(out, n, "%04x/%04x", d.pan, d.shortAddr);
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
    } else {
        fmtMac(out, n, hunt.mac);
    }
}

void sendHello() {
    // 900, not 780: "both" mode (38 channels) + an active hunt + a targeted deauth + an SD path summed to
    // ~790, and a line that passes the check and then overruns is truncated mid-JSON, which is exactly what
    // the drop-whole-lines rule exists to prevent.
    if (!serialRoom(900)) return;
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
    char mac[26];
    huntIdText(mac, sizeof(mac));
    static const char* const kRst[] = {"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
                                       "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"};
    const int rr = static_cast<int>(esp_reset_reason());
    Serial.printf("],\"park\":%d,\"cap\":%d,\"snap\":%u,\"heap\":%u,\"up\":%lu,\"rst\":\"%s\",\"hunt\":%s%s%s,",
                  parkedIdx >= 0 ? kChannels[parkedIdx] : 0, captureEnabled ? 1 : 0,
                  static_cast<unsigned>(capSnapLen), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(millis() / 1000), (rr >= 0 && rr < 16) ? kRst[rr] : "?",
                  hunt.active ? "\"" : "null", hunt.active ? mac : "", hunt.active ? "\"" : "");
    printHunt();
    Serial.print(",");
    printDeauth();
    Serial.printf(",\"sd\":{\"mounted\":%d,\"mb\":%lu,\"cap\":%d,\"file\":\"%s\",\"frames\":%lu,\"bytes\":%lu,\"err\":%lu,\"clock\":%d}",
                  sd.cardPresent ? 1 : 0, static_cast<unsigned long>(sd.cardMb),
                  sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "",
                  static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes),
                  static_cast<unsigned long>(sd.errors), epochValid ? 1 : 0);
    Serial.printf(",\"mir\":%d", g_mirror ? 1 : 0);
    Serial.print("}\n");
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
        // Frequency is lo + i*step (not sent per bin). Up to 84 bins (~1.7 kB) — dropped whole if no room.
        const int nb = specBinCount();
        if (!serialRoom(static_cast<size_t>(nb) * 22 + 80)) return;
        Serial.printf("{\"t\":\"fs\",\"n\":%lu,\"step\":%u,\"lo\":%d,\"count\":%d,\"bins\":[",
                      static_cast<unsigned long>(sweepCount), specStepMhz, kSpecLoMhz, nb);
        for (int i = 0; i < nb; i++) {
            const SpecBin& b = specFine[i];
            Serial.printf("%s[%d,%d,%d,%u]", i ? "," : "", b.edMin, b.edMean, b.edMax, b.edSamples);
        }
        Serial.printf("],\"heap\":%u}\n", static_cast<unsigned>(ESP.getFreeHeap()));
        return;
    }
    if (!serialRoom(1500)) return;
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
    for (int i = 0; i < kBleDevSlots; i++) if (bleDevs[i].lastMs && now - bleDevs[i].lastMs <= kDevFreshMs) n++;
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
        if (!serialRoom(40 + n * 80)) return;
        // [id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]
        Serial.print("{\"t\":\"z\",\"dev\":[");
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            Dev154 d;
            if (!fetch154Dev(refs[i], d, kDevFreshMs)) continue;   // slot reused/evicted since collect: skip
            fmtKey154(mac, sizeof(mac), d);
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,%u,%u,%u,%u]", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                          static_cast<unsigned long>(now - d.lastMs), d.ch, d.pan, d.shortAddr, d.proto, d.flags, d.lqi);
        }
        Serial.print("]}\n");
        return;
    }
    if (wifiMode()) {
        const int n = collectWifiRefs(refs, kWifiDevSlots, kDevFreshMs);
        // Sent in chunks of kWifiRowsPerLine rows, loudest first: a full 96-slot table (~12 kB) cannot fit the 8 KB
        // TX buffer as one line. Each chunk is budgeted on its own (+8 per row: the association suffix field added
        // in 1.5.5), and the host merges rows by MAC, so a chunk that does not fit this cycle (the quietest
        // devices, last in RSSI order) just waits for the next one. Every chunk is a complete "w" line.
        constexpr int kWifiRowsPerLine = 24;
        for (int base = 0; base < n || (base == 0 && n == 0); base += kWifiRowsPerLine) {
            const int end = LV_MIN(n, base + kWifiRowsPerLine);
            if (!serialRoom(40 + (end - base) * 126)) return;
            Serial.print("{\"t\":\"w\",\"dev\":[");
            int emitted = 0;
            for (int i = base; i < end; i++) {
                WifiDev d;
                if (!fetchWifiDev(refs[i], d, kDevFreshMs)) continue;
                fmtMac(mac, sizeof(mac), d.mac);
                Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.frames,
                              static_cast<unsigned long>(now - d.lastMs), d.ch, d.flags);
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
        // A maxed row is ~110 B (int8 -128s, uint16 65535s, a 20-char name) plus escapes in the name; an
        // under-budget row truncates the line mid-write instead of dropping it whole. 48 x 116 + 40 = 5.6 kB.
        if (!serialRoom(40 + n * 116)) return;
        Serial.print("{\"t\":\"b\",\"dev\":[");
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            BleDev d;
            if (!fetchBleDev(refs[i], d, kDevFreshMs)) continue;
            fmtMac(mac, sizeof(mac), d.mac);
            Serial.printf("%s[\"%s\",%d,%d,%u,%lu,%u,%u,", emitted++ ? "," : "", mac, d.rssi, d.maxRssi, d.adv,
                          static_cast<unsigned long>(now - d.lastMs), d.addrType, d.company);
            printJsonStr(d.name);
            Serial.printf(",%u,%d,%u,%u,%u,%u,%u]", d.appearance, d.txPower, d.svc, d.svcData, d.appleType,
                          d.flags, d.surv);
        }
        Serial.print("]}\n");
    }
}

namespace {

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

void handleCommand(char* line) {
    // Commands: "cap 0|1", "snap N", "park <ch>|0", "band 5g|2.4g|both|ble", "hunt <mac> [ch]" | "hunt 0",
    //           "deauth <bssid>" | "deauth 0" (Wi-Fi modes only, kicks every station),
    //           "dca <client_mac> <ap_bssid>" | "dca 0" (targeted: one station), "info"
    char* sp = strchr(line, ' ');
    char* arg = const_cast<char*>("");
    if (sp) { *sp = 0; arg = sp + 1; }
    if (!strcmp(line, "cap")) {
        // Wi-Fi / 802.15.4 / BLE all have a link type; spec arms no RX, so refuse rather than record silence.
        const bool want = atoi(arg) != 0;
        const bool on = want && !modeSpec() && ensureCapRing();
        captureEnabled = on;
        syncCapActive();
        if (want && modeSpec()) Serial.printf("{\"t\":\"log\",\"msg\":\"cap: no RX armed in spec mode\"}\n");
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"cap\",\"cap\":%d}\n", captureEnabled ? 1 : 0);
    } else if (!strcmp(line, "sdcap")) {
        const bool want = atoi(arg) != 0;
        if (want && modeSpec() && !sd.capEnabled) {   // no RX armed in spec: the file would never grow
            Serial.printf("{\"t\":\"err\",\"msg\":\"sdcap: no RX armed in spec mode\"}\n");
        } else if (want && !sd.capEnabled) {
            // Open the file first: mounting FATFS costs ~30 KB, and the ring must be sized against what is
            // left afterwards or kCapHeapReserve is not actually reserved. A ring that "cap 1" already made
            // was sized before the mount, so refit re-sizes it if the mount pushed heap under the floor.
            if (!sdOpenCapture()) {
                Serial.printf("{\"t\":\"err\",\"msg\":\"sdcap: %s\"}\n",
                              sd.mounted ? "could not open file on card" : "no SD card (check it is inserted)");
            } else if (!refitCapRing()) {
                Serial.print("{\"t\":\"err\",\"msg\":\"sdcap: no capture ring\"}\n");
                sdCloseCapture();
            } else if (serialRoom(140)) {
                Serial.printf("{\"t\":\"log\",\"msg\":\"sd capture -> %s\"}\n", sd.path);
            }
        } else if (!want) {
            sdCloseCapture();
        }
        syncCapActive();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdcap\",\"sdcap\":%d,\"file\":\"%s\"}\n",
                      sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "");
    } else if (!strcmp(line, "sdinfo")) {
        const bool m = sdMount();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"sdinfo\",\"sd\":%d,\"mb\":%lu,\"used_mb\":%lu,\"cap\":%d,\"file\":\"%s\","
                      "\"frames\":%lu,\"bytes\":%lu,\"err\":%lu}\n",
                      m ? 1 : 0, m ? static_cast<unsigned long>(SD.cardSize() / (1024 * 1024)) : 0UL,
                      m ? static_cast<unsigned long>(SD.usedBytes() / (1024 * 1024)) : 0UL,
                      sd.capEnabled ? 1 : 0, sd.capEnabled ? sd.path : "",
                      static_cast<unsigned long>(sd.frames), static_cast<unsigned long>(sd.bytes),
                      static_cast<unsigned long>(sd.errors));
    } else if (!strcmp(line, "sdls")) {
        sdListFiles();
    } else if (!strcmp(line, "sdread")) {
        sdReadFile(arg);
    } else if (!strcmp(line, "addr1")) {
        trackAddr1 = atoi(arg) != 0;
        saveSettings();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"addr1\",\"addr1\":%d}\n", trackAddr1 ? 1 : 0);
    } else if (!strcmp(line, "blescan")) {
        if (!strcmp(arg, "active"))       bleScan.mode = BLE_SCAN_ACTIVE;
        else if (!strcmp(arg, "passive")) bleScan.mode = BLE_SCAN_PASSIVE;
        else if (!strcmp(arg, "auto"))    bleScan.mode = BLE_SCAN_AUTO;
        bleScan.lastSwitchMs = 0;   // apply the new policy on the next serviceBle() without waiting out the limit
        saveSettings();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"blescan\",\"mode\":\"%s\",\"running\":\"%s\"}\n",
                      bleScanModeName(), bleScan.active ? "active" : "passive");
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
        saveSettings();
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"snap\",\"snap\":%d}\n", n);
    } else if (!strcmp(line, "park")) {
        const int ch = atoi(arg);
        const int idx = (ch > 0) ? indexOfChannel(ch) : -1;
        setPark((idx >= 0 && chanEnabled(idx)) ? idx : -1);
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"park\",\"park\":%d}\n", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
    } else if (!strcmp(line, "band")) {
        const BandMode prev = bandMode;
        if (!strcmp(arg, "5g")) setBandMode(BAND_5G);
        else if (!strcmp(arg, "2.4g") || !strcmp(arg, "24g")) setBandMode(BAND_24G);
        else if (!strcmp(arg, "both")) setBandMode(BAND_BOTH);
        else if (!strcmp(arg, "ble")) setBandMode(BAND_BLE);
        else if (!strcmp(arg, "154") || !strcmp(arg, "zigbee") || !strcmp(arg, "thread")) setBandMode(BAND_154);
        else if (!strcmp(arg, "spec") || !strcmp(arg, "spectrum")) setBandMode(BAND_SPEC);
        if (modeSpec()) showPage(PAGE_SPECTRUM);
        else if (!hopMode() && (currentPage == PAGE_OVERVIEW || currentPage == PAGE_CHANNELS)) showPage(PAGE_DEVICES);
        if (bandMode != prev) showBandSplash(bandMode, kSplashShowMs);   // name the new mode before its scan page
        if (bandMode != prev) saveSettings();   // persist the new mode (C3); park is intentionally not saved
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"band\",\"band\":\"%s\"}\n", kBandName[bandMode]);
        sendHello();
    } else if (!strcmp(line, "hunt")) {
        uint8_t mac[6];
        char* sp2 = strchr(arg, ' ');
        int ch = 0;
        if (sp2) { *sp2 = 0; ch = atoi(sp2 + 1); }
        uint8_t key[8];
        if (parseMac(arg, mac)) startHunt(mac, ch);
        else if (parseKey154(arg, key)) { startHunt154(key); if (ch > 0) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); hunt.parked = true; } } }
        else stopHunt();
        char m[26];
        huntIdText(m, sizeof(m));
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":%s%s%s,\"park\":%d}\n", hunt.active ? "\"" : "null",
                      hunt.active ? m : "", hunt.active ? "\"" : "", parkedIdx >= 0 ? kChannels[parkedIdx] : 0);
        if (hunt.active && hunt.kind == 1 && mode154()) {
            // park on the channel the node was last seen on
            portENTER_CRITICAL(&g_devMux);
            int ch = 0;
            for (int i = 0; i < kDev154Slots; i++) if (devs154[i].lastMs && key8Eq(devs154[i].key, hunt.key)) { ch = devs154[i].ch; break; }
            portEXIT_CRITICAL(&g_devMux);
            if (ch && parkedIdx < 0) { const int idx = indexOfChannel154(ch); if (idx >= 0) { setPark(idx); hunt.parked = true; } }
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
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":[\"%s\",%d,0,0],\"park\":%d,\"fc\":\"0x%02x\"}\n",
                          m, ch, ch, kickFc);
        else
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"deauth\",\"deauth\":null,\"park\":%d}\n", ch);
        if (deauth.active && serialRoom(140))   // DIAGNOSTIC: hmac slot used by the internal path + its state byte (picks the DA/SA mapping)
            Serial.printf("{\"t\":\"log\",\"msg\":\"deauth slot %lx hstate %d\"}\n", *(const uint32_t*)deauth.slotPad, deauth.hstate);
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
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"dca\",\"deauth\":[\"%s\",\"%s\",%d,0,0],\"park\":%d,\"fc\":\"0x%02x\"}\n",
                          c, a, deauth.targeted ? 1 : 0, ch, kickFc);
        else
            Serial.printf("{\"t\":\"ack\",\"cmd\":\"dca\",\"deauth\":null,\"park\":%d}\n", ch);
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
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"kickfc\",\"fc\":\"0x%02x\"}\n", kickFc);
    } else if (!strcmp(line, "kickpath")) {
        // 0 (default) = raw esp_wifi_80211_tx, the only path measured to reach the air (§11).
        // 1 = driver-internal slot, which transmits nothing for any subtype; kept for §9 offset work.
        useInternalKick = atoi(arg) != 0;
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"kickpath\",\"internal\":%d}\n", useInternalKick ? 1 : 0);
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
    } else if (!strcmp(line, "txstat")) {
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"txstat\",\"sent\":%lu,\"fail\":%lu,\"ch\":%u}\n",
                      static_cast<unsigned long>(txTestSent), static_cast<unsigned long>(txTestFail), currentChannelNum);
    } else if (!strcmp(line, "specstep")) {
        const int st = atoi(arg);
        if (st == 1 || st == 2 || st == 5) {
            specStepMhz = static_cast<uint8_t>(st);
            if (modeSpec()) { resetChannelStats(); currentIdx = -1; monitorReady = advanceChannel(); }
            saveSettings();
        }
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"specstep\",\"step\":%u}\n", specStepMhz);
    } else if (!strcmp(line, "mirror")) {
        g_mirror = (atoi(arg) != 0);
        if (g_mirror) mirrorRequestFull();   // push a full frame now (paced across the next loops)
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"mirror\",\"mirror\":%d,\"w\":%d,\"h\":%d}\n",
                      g_mirror ? 1 : 0, LCD_WIDTH, LCD_HEIGHT);
    } else if (!strcmp(line, "page")) {
        stepPage(!strcmp(arg, "prev") ? -1 : 1);   // "next"/empty = forward, like a BOOT tap
        Serial.printf("{\"t\":\"ack\",\"cmd\":\"page\",\"page\":%d}\n", currentPage);
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

} // namespace

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
