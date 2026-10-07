#pragma once
// Device tracking shared between the Wi-Fi promiscuous path, the BLE scan callback, the UI and the host
// protocol. Tables are fixed-size open-addressing hash tables keyed by MAC; updates happen in the radio
// tasks under a spinlock, readers take a snapshot under the same lock.
#include <stdint.h>

constexpr int kWifiDevSlots = 96;   // 1.17: was 64 (+2 kB static); "w" lines are chunked to fit (host_proto.cpp)
constexpr int kBleDevSlots = 48;
constexpr int kDevProbe = 8;

// Field order here is deliberate and load-bearing: it packs to exactly 64 bytes with no padding, which
// is what let apSuffix be added in 1.5.4 at zero static-RAM cost (wifiDevs holds kWifiDevSlots of these, so a
// byte of padding costs 96 x 4 = 384 B once alignment rounds the struct up). Re-check the static_assert after any edit.
struct WifiDev {
    uint32_t lastMs;     // 0 = empty slot
    uint8_t mac[6];
    int8_t rssi;
    int8_t maxRssi;
    uint16_t frames;
    uint16_t stations;   // BSS load station count
    // Last 3 bytes of the BSSID this device is associated with, from the DS bits of a data frame
    // (ToDS: BSSID is addr1; FromDS: BSSID is addr2). Zero = never seen associated. Only the suffix is
    // kept because the full 6 bytes would push this struct to 68 and cost 512 B of static RAM; the host
    // joins it against APs it already knows, so a match needs a *known* AP whose low 24 bits agree.
    uint8_t apSuffix[3];
    uint8_t ch;
    uint8_t flags;       // bit0: AP (beacon/probe resp), bit1: IEs parsed, bit2: seen ONLY as a frame
                         //   destination (addr1) - we have never heard it transmit, so rssi/ch belong to
                         //   whoever addressed it. Tier 1 evidence; cleared as soon as it transmits.
    uint8_t surv;        // SurvCat: known surveillance hardware by OUI, 0 = none
    // From beacon / probe-response information elements (APs only)
    uint8_t sec;         // bit0 WEP, bit1 WPA1, bit2 WPA2-PSK, bit3 WPA2-ENT, bit4 WPA3-SAE, bit5 WPA3-ENT, bit6 OWE, bit7 open
    uint8_t pmf;         // 0 none, 1 capable, 2 required (802.11w)
    uint8_t phy;         // bit0 legacy(a/b/g), bit1 n (HT), bit2 ac (VHT), bit3 ax (HE), bit4 be (EHT)
    uint8_t bw;          // channel width in units of 10 MHz (2, 4, 8, 16)
    uint8_t util;        // BSS load channel utilisation, 0-255 (255 = 100 %), 0 if not advertised
    uint8_t beacons;     // beacon counter (IEs re-parsed every 16th)
    char cc[3];          // country IE
    char ssid[33];
};
static_assert(sizeof(WifiDev) == 64, "WifiDev must stay 64 bytes: see the packing note above");

// Field order is load-bearing, like WifiDev's: the 4-byte field leads, then the 2-byte ones, then bytes, so it
// packs to exactly 48 B with no padding (the 1.4 order was 56 B with 8 B of padding). 48 x kBleDevSlots = 2,304 B.
struct BleDev {
    uint32_t lastMs;     // 0 = empty slot
    uint8_t mac[6];
    int8_t rssi;
    int8_t maxRssi;
    uint16_t adv;
    uint16_t company;    // Bluetooth SIG company id from manufacturer data, 0 if none
    uint16_t appearance; // GAP appearance, 0 if none
    uint16_t svc;        // first 16-bit service UUID advertised, 0 if none
    uint16_t svcData;    // first 16-bit service-data UUID, 0 if none
    uint8_t addrType;    // 0 public, 1 random
    int8_t txPower;      // advertised TX power, 127 if none
    uint8_t appleType;   // Apple manufacturer-data type byte (0x12 FindMy, 0x07 AirPods, 0x0c handoff, ...), 0 if n/a
    uint8_t flags;       // bit0 connectable, bit1 legacy advertisement, bit2 scannable
    uint8_t surv;        // SurvCat: known surveillance hardware by OUI, 0 = none
    char name[21];
};
static_assert(sizeof(BleDev) == 48, "BleDev must stay 48 bytes: repack before growing it (see the order note above)");

// IEEE 802.15.4 (Zigbee / Thread) node, keyed by extended (64-bit) address when the frame carried one,
// otherwise by PAN id + short address (key bytes 0..1 = 0xFF 0xFE marker, 2..3 pan, 4..5 short).
constexpr int kDev154Slots = 48;
struct Dev154 {
    uint8_t key[8];
    uint16_t shortAddr;  // 0xFFFF = unknown
    uint16_t pan;        // 0xFFFF = unknown / broadcast
    int8_t rssi;
    int8_t maxRssi;
    uint16_t frames;
    uint32_t lastMs;     // 0 = empty slot
    uint8_t ch;
    uint8_t lqi;
    uint8_t proto;       // 0 unknown, 1 Zigbee, 2 Zigbee Green Power, 3 Thread / 6LoWPAN, 4 MAC-secured (likely Thread)
    uint8_t flags;       // bit0 extended address known, bit1 sends beacons (coordinator/router), bit2 permit-join, bit3 MAC security, bit4 data seen, bit5 ack seen
};
static_assert(sizeof(Dev154) == 24, "Dev154 must stay 24 bytes: repack before growing it");

inline bool key8Eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 8; i++) if (a[i] != b[i]) return false;
    return true;
}
inline int dev154FindSlot(Dev154* tab, int slots, const uint8_t* key) {
    const int start = (key[7] * 31 + key[6] * 17 + key[5] * 7 + key[4] * 3 + key[2]) % slots;
    int oldest = -1;
    uint32_t oldestMs = 0xFFFFFFFFu;
    for (int p = 0; p < kDevProbe; p++) {
        const int i = (start + p) % slots;
        if (tab[i].lastMs == 0) return i;
        if (key8Eq(tab[i].key, key)) return i;
        if (tab[i].lastMs < oldestMs) { oldestMs = tab[i].lastMs; oldest = i; }
    }
    return oldest >= 0 ? oldest : start;   // never -1: every probed slot could hold lastMs == UINT32_MAX
}

inline int macHashIdx(const uint8_t* mac, int slots) {
    return (mac[3] * 31 + mac[4] * 17 + mac[5] * 7 + mac[2]) % slots;
}

inline bool macEq(const uint8_t* a, const uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

// Find the slot for mac (existing, empty, or the oldest within the probe window). Returns index.
template <typename T>
inline int devFindSlot(T* tab, int slots, const uint8_t* mac) {
    const int start = macHashIdx(mac, slots);
    int oldest = -1;
    uint32_t oldestMs = 0xFFFFFFFFu;
    for (int p = 0; p < kDevProbe; p++) {
        const int i = (start + p) % slots;
        if (tab[i].lastMs == 0) return i;               // empty: insert here
        if (macEq(tab[i].mac, mac)) return i;           // found
        if (tab[i].lastMs < oldestMs) { oldestMs = tab[i].lastMs; oldest = i; }
    }
    return oldest >= 0 ? oldest : start;                // evict the stalest probed entry (never -1)
}
