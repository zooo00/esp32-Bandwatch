#pragma once
// Known surveillance hardware, matched on the first three MAC bytes. Sources: colonelpanichacks/
// ouispy-detector (ouis.md) and the Flock Safety prefixes researched by OrdoOuroboros / @NitekryDPaul
// via flock-you. An OUI match is evidence, not proof: prefixes get reassigned, and two entries in the
// Flock set were withdrawn upstream as Ubiquiti false positives. Treat a hit as "worth a look".
#include <stdint.h>

enum SurvCat : uint8_t { SURV_NONE = 0, SURV_FLOCK = 1, SURV_RING = 2, SURV_AXON = 3,
                         SURV_DJI = 4, SURV_PARROT = 5, SURV_SKYDIO = 6, SURV_META = 7 };
// Names indexed by SurvCat; the host keeps its own copy (SURV_CAT in bandwatch_host.py) — keep in sync.
constexpr const char* kSurvName[] = {"", "Flock Safety", "Ring", "Axon", "DJI", "Parrot", "Skydio", "Meta/Ray-Ban"};
struct SurvOui { uint8_t o[3]; uint8_t cat; };
constexpr SurvOui kSurvOuis[] = {
    {{0x00, 0xF4, 0x8D}, 1},   // FLOCK
    {{0x08, 0x3A, 0x88}, 1},   // FLOCK
    {{0x14, 0x5A, 0xFC}, 1},   // FLOCK
    {{0x14, 0xB5, 0xCD}, 1},   // FLOCK
    {{0x24, 0xB2, 0xB9}, 1},   // FLOCK
    {{0x3C, 0x71, 0xBF}, 1},   // FLOCK
    {{0x3C, 0x91, 0x80}, 1},   // FLOCK
    {{0x48, 0x27, 0xEA}, 1},   // FLOCK
    {{0x58, 0x00, 0xE3}, 1},   // FLOCK
    {{0x58, 0x8E, 0x81}, 1},   // FLOCK
    {{0x5C, 0x93, 0xA2}, 1},   // FLOCK
    {{0x64, 0x6E, 0x69}, 1},   // FLOCK
    {{0x70, 0x08, 0x94}, 1},   // FLOCK
    {{0x70, 0xC9, 0x4E}, 1},   // FLOCK
    {{0x74, 0x4C, 0xA1}, 1},   // FLOCK
    {{0x80, 0x30, 0x49}, 1},   // FLOCK
    {{0x82, 0x6B, 0xF2}, 1},   // FLOCK
    {{0x90, 0x35, 0xEA}, 1},   // FLOCK
    {{0x94, 0x08, 0x53}, 1},   // FLOCK
    {{0x9C, 0x2F, 0x9D}, 1},   // FLOCK
    {{0xA4, 0xCF, 0x12}, 1},   // FLOCK
    {{0xB4, 0x1E, 0x52}, 1},   // FLOCK
    {{0xB8, 0x1E, 0xA4}, 1},   // FLOCK
    {{0xB8, 0x35, 0x32}, 1},   // FLOCK
    {{0xC0, 0x35, 0x32}, 1},   // FLOCK
    {{0xD0, 0x39, 0x57}, 1},   // FLOCK
    {{0xD8, 0xF3, 0xBC}, 1},   // FLOCK
    {{0xE0, 0x0A, 0xF6}, 1},   // FLOCK
    {{0xE0, 0x4F, 0x43}, 1},   // FLOCK
    {{0xE4, 0xAA, 0xEA}, 1},   // FLOCK
    {{0xE8, 0xD0, 0xFC}, 1},   // FLOCK
    {{0xEC, 0x1B, 0xBD}, 1},   // FLOCK
    {{0xF4, 0x6A, 0xDD}, 1},   // FLOCK
    {{0x18, 0x7F, 0x88}, 2},   // RING
    {{0x24, 0x2B, 0xD6}, 2},   // RING
    {{0x34, 0x3E, 0xA4}, 2},   // RING
    {{0x54, 0xE0, 0x19}, 2},   // RING
    {{0x5C, 0x47, 0x5E}, 2},   // RING
    {{0x64, 0x9A, 0x63}, 2},   // RING
    {{0x90, 0x48, 0x6C}, 2},   // RING
    {{0x9C, 0x76, 0x13}, 2},   // RING
    {{0xAC, 0x9F, 0xC3}, 2},   // RING
    {{0xC4, 0xDB, 0xAD}, 2},   // RING
    {{0xCC, 0x3B, 0xFB}, 2},   // RING
    {{0x00, 0x25, 0xDF}, 3},   // AXON
    {{0x04, 0xA8, 0x5A}, 4},   // DJI
    {{0x0C, 0x9A, 0xE6}, 4},   // DJI
    {{0x34, 0xD2, 0x62}, 4},   // DJI
    {{0x48, 0x1C, 0xB9}, 4},   // DJI
    {{0x58, 0xB8, 0x58}, 4},   // DJI
    {{0x60, 0x60, 0x1F}, 4},   // DJI
    {{0x8C, 0x58, 0x23}, 4},   // DJI
    {{0xE4, 0x7A, 0x2C}, 4},   // DJI
    {{0x00, 0x12, 0x1C}, 5},   // PARROT
    {{0x00, 0x26, 0x7E}, 5},   // PARROT
    {{0x90, 0x03, 0xB7}, 5},   // PARROT
    {{0x90, 0x3A, 0xE6}, 5},   // PARROT
    {{0xA0, 0x14, 0x3D}, 5},   // PARROT
    {{0x38, 0x1D, 0x14}, 6},   // SKYDIO
    {{0x5C, 0xE9, 0x1E}, 7},   // META
    {{0x7C, 0x2A, 0x9E}, 7},   // META
    {{0x98, 0x59, 0x49}, 7},   // META
    {{0xCC, 0x66, 0x0A}, 7},   // META
    {{0xF4, 0x03, 0x43}, 7},   // META
};
constexpr int kSurvOuiCount = sizeof(kSurvOuis) / sizeof(kSurvOuis[0]);

constexpr int kSurvNames = sizeof(kSurvName) / sizeof(kSurvName[0]);

// Extra OUIs read from /surveil.csv on the card at boot (events.cpp, loadSurvExtra); nullptr/0 without one.
extern SurvOui* g_survExtra;
extern int g_survExtraN;

inline uint8_t survLookup(const uint8_t* mac) {
    for (int i = 0; i < kSurvOuiCount; i++)
        if (kSurvOuis[i].o[0] == mac[0] && kSurvOuis[i].o[1] == mac[1] && kSurvOuis[i].o[2] == mac[2])
            return kSurvOuis[i].cat;
    for (int i = 0; i < g_survExtraN; i++)
        if (g_survExtra[i].o[0] == mac[0] && g_survExtra[i].o[1] == mac[1] && g_survExtra[i].o[2] == mac[2])
            return g_survExtra[i].cat;
    return SURV_NONE;
}
