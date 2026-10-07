// IEEE 802.15.4 (Zigbee / Thread) sniffing for Bandwatch. The driver's callbacks run in ISR context, so the RX
// path is IRAM_ATTR and spinlock-guarded - no heap, no Serial.
#include "bandwatch_core.h"

#include <esp_ieee802154.h>
#include "hal/ieee802154_common_ll.h"   // POC: ieee802154_ll_set_freq() tunes the synth below the channel API

bool r154Running = false;   // instance lives with its module; core reads it via bandwatch_core.h
bool specRunning = false;   // energy-detect spectrum sweep up (15.4 radio powered, no RX armed)

// Energy-detect accumulator (BAND_SPEC): esp_ieee802154_energy_detect_done() folds raw dBm samples in here
// in driver context; finishDwell() snapshots it at dwell end. Guarded by its own spinlock.
static portMUX_TYPE g_edMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t s_edSum = 0;
static volatile int16_t s_edMin = 127;
static volatile int16_t s_edMax = -128;
static volatile uint16_t s_edN = 0;
static volatile bool s_edReady = false;   // one ED finished; re-arm from the loop task, not the ISR

namespace {   // locals; closed before the driver callback because it needs C linkage
// Per-dwell counting goes through g_accum (bandwatch.cpp); nodes go into devs154.

// Classify an unsecured MAC payload: Zigbee NWK header, Zigbee Green Power, or 6LoWPAN (Thread). MAC-secured
// frames (proto 4, likely Thread) are tagged by the caller, which has the security bit.
inline uint8_t IRAM_ATTR classify154(const uint8_t* pl, uint16_t n) {
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
    if ((flagBits & 4) && !(d.flags & 4)) g_ledJoinFlag = true;   // C10: permit-join first seen on this node (ISR: flag only)
    d.flags |= flagBits;
    noteHuntHit(hunt.kind == 1 && key8Eq(hunt.key, key), rssi, now);
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
        // Only 2003/2006 frames (version 0/1) are address-parsed into the node table. 2015+ frames (version 2)
        // can omit PAN ids and carry IEs, so this layout would misread them: they still count toward the
        // channel and are captured, but get no node row, beacon info or LQI. Most Zigbee/Thread traffic is v0/v1.
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
                    if (!secured && off < n) proto = classify154(p + off, n - off);
                    else if (secured) { proto = 4; flagBits |= 8; }
                } else if (ftype == 3) {                // MAC command
                    if (secured) flagBits |= 8;
                }
                track154(key, hasExt, srcPan, shortAddr, rssi, info->lqi, proto, flagBits);
            }
        }

        uint8_t nh;
        uint16_t room;
        CapFrame* slot = capReserve(nh, room);
        if (slot) {
            uint16_t c = n;
            if (c > capSnapLen) c = capSnapLen;
            if (c > room) c = room;
            slot->ts_us = static_cast<uint32_t>(info->timestamp);
            slot->len = n;
            slot->capLen = c;
            slot->rssi = rssi;
            slot->channel = currentChannelNum;
            memcpy(slot->data, p, c);
            capCommit(nh);
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

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
// Spectrum (energy-detect) mode. Powers the 15.4 radio but never arms RX: instead each dwell runs a
// stream of energy-detect windows on the current channel, giving a raw dBm reading with no packet decode.
// ---------------------------------------------------------------------------------------------
void edReset() {
    portENTER_CRITICAL(&g_edMux);
    s_edSum = 0; s_edMin = 127; s_edMax = -128; s_edN = 0;
    portEXIT_CRITICAL(&g_edMux);
    s_edReady = false;
}

void edKick() { esp_ieee802154_energy_detect(kEdDurationSym); }

// POC: tune the 15.4 synthesiser to an arbitrary MHz (the channel register's freq field is MHz-2400, 7 bits),
// so energy detect can sweep off the standard 5 MHz channel grid. Validity of off-grid reads is TBD.
void edSetFreqMhz(int mhz) { ieee802154_ll_set_freq(static_cast<uint8_t>(mhz - 2400)); }

// Re-arm the next energy-detect window from the loop task. Arming is not safe from the done callback (ISR)
// — doing it there yields exactly one sample per dwell — so the callback only flags completion and the loop
// kicks the next one. Each window is ~2 ms (kEdDurationSym), so a kEdDwellMs (60 ms) dwell folds in ~25 samples.
void serviceSpectrum() {
    if (specRunning && s_edReady) {
        s_edReady = false;
        esp_ieee802154_energy_detect(kEdDurationSym);
    }
}

void edSnapshot(int8_t& mn, int8_t& mx, int8_t& mean, uint16_t& n) {
    portENTER_CRITICAL(&g_edMux);
    n = s_edN;
    if (s_edN) {
        mn = static_cast<int8_t>(s_edMin);
        mx = static_cast<int8_t>(s_edMax);
        mean = static_cast<int8_t>(s_edSum / static_cast<int32_t>(s_edN));
    } else {
        mn = 0; mx = -128; mean = 0;
    }
    portEXIT_CRITICAL(&g_edMux);
}

void startSpectrum() {
    if (specRunning) return;
    esp_ieee802154_enable();
    // No set_promiscuous / receive(): the RX done callback never fires. advanceChannel()'s spec branch arms
    // the first energy-detect window (edReset + edKick) once a frequency is parked.
    edReset();
    specRunning = true;
}

void stopSpectrum() {
    if (!specRunning) return;
    specRunning = false;            // stop the done callback re-arming before we power the radio down
    esp_ieee802154_sleep();
    esp_ieee802154_disable();
    currentChannelNum = 0;
}

// Energy-detect result: raw channel energy in dBm. Fold it in and flag that the loop may re-arm the next
// window (arming here, in driver/ISR context, does not work — see serviceSpectrum). applyChannelIdx() resets
// the accumulator on each new channel. IRAM_ATTR and spinlock-guarded like the RX callback.
extern "C" void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power) {
    if (!specRunning) return;
    portENTER_CRITICAL_ISR(&g_edMux);
    s_edSum += power;
    if (power < s_edMin) s_edMin = power;
    if (power > s_edMax) s_edMax = power;
    if (s_edN < 65535) s_edN += 1;
    portEXIT_CRITICAL_ISR(&g_edMux);
    s_edReady = true;
}
