// Settings persistence for Bandwatch (C3). One NVS namespace holds the handful of settings a walk-around
// device should survive a power-cycle with, so it boots ready instead of needing the host to re-talk it
// through serial every time. Transient RAM only: the Preferences handle opens for the load/save and is
// closed immediately, so nothing stays resident (the §16 budget is tight - see docs/DEVELOPER.md).
//
// Persisted: band mode + the scalar policies (addr1, blescan, specstep, snap).
// NOT persisted, on purpose:
//   - park: a device that boots silently parked on one channel is a "why is it stuck" footgun; let it hop.
//   - hunt / deauth: a reboot must STOP transmitting (the same safety argument as the dead-man's switch),
//     and must not resume an attack nobody is watching.
#include "bandwatch_core.h"
#include <Preferences.h>

BandMode g_restoredMode = BAND_5G;
bool     g_settingsRestored = false;

namespace {
constexpr char kNs[]  = "bandwatch";   // NVS namespace
constexpr uint8_t kSchema = 1;         // bump on an incompatible key change; a mismatch falls back to defaults

Preferences prefs;
}

// Restore settings into the scalar globals and g_restoredMode. Safe to call before any radio is up; it only
// touches globals and the (closed-after) NVS handle. Missing/corrupt/old-schema NVS leaves today's defaults.
void loadSettings() {
    if (!prefs.begin(kNs, /*readOnly=*/true)) return;   // namespace never written yet: keep compile defaults
    if (prefs.getUChar("ver", 0) != kSchema) { prefs.end(); return; }

    const uint8_t  band = prefs.getUChar("band", BAND_5G);
    const uint8_t  a1   = prefs.getUChar("addr1", trackAddr1 ? 1 : 0);
    const uint8_t  bs   = prefs.getUChar("blescan", bleScan.mode);
    const uint8_t  ss   = prefs.getUChar("specstep", specStepMhz);
    const uint16_t sn   = prefs.getUShort("snap", capSnapLen);
    prefs.end();

    // Validate every value: NVS is off-device state and a corrupt byte must not reach the radio paths.
    if (band < kBandModes)                         g_restoredMode = static_cast<BandMode>(band);
    trackAddr1  = (a1 != 0);
    if (bs <= BLE_SCAN_AUTO)                        bleScan.mode = static_cast<BleScanMode>(bs);
    if (ss == 1 || ss == 2 || ss == 5)             specStepMhz  = ss;
    if (sn >= 32 && sn <= kCapMaxLen)              capSnapLen   = sn;
    g_settingsRestored = true;
}

// Write the current settings. Called on a user commit (serial command or a committed button walk), not on
// every walk-step, so flash wear and mid-walk latency stay negligible. Runs on the loop task only.
void saveSettings() {
    if (!prefs.begin(kNs, /*readOnly=*/false)) return;
    prefs.putUChar("ver", kSchema);
    prefs.putUChar("band", bandMode);
    prefs.putUChar("addr1", trackAddr1 ? 1 : 0);
    prefs.putUChar("blescan", bleScan.mode);
    prefs.putUChar("specstep", specStepMhz);
    prefs.putUShort("snap", capSnapLen);
    prefs.end();
}
