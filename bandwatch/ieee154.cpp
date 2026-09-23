// IEEE 802.15.4 (Zigbee / Thread) sniffing for Bandwatch. The driver's callbacks run in ISR context, so the RX
// path is IRAM_ATTR and spinlock-guarded - no heap, no Serial.
#include "bandwatch_core.h"

#include <esp_ieee802154.h>

bool r154Running = false;   // instance lives with its module; core reads it via bandwatch_core.h

namespace {   // locals; closed before the driver callback because it needs C linkage
// No globals needed here — all counting goes through g_accum in capture.cpp

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

        uint8_t nh;
        CapFrame* slot = capReserve(nh);
        if (slot) {
            uint16_t c = n;
            if (c > capSnapLen) c = capSnapLen;
            if (c > kCapMaxLen) c = kCapMaxLen;
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
