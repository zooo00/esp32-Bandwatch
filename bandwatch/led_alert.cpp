// LED alert blips for Bandwatch (D1: 1.6.1 surveillance alerting, C4 novelty, C10 permit-join). docs/DEVELOPER.md §21.
//
// The single WS2812 already carries a permanent state picked by driveLed() (lcd_ui.cpp): deauth blink > hunt
// distance > record pulse > busy score. An alert is NOT a new permanent state: it wins the LED for ~300-500 ms with
// a distinct on/off pattern, then driveLed() takes the LED back on its next 120 ms UI tick.
//
//   kind        pattern (50 ms slots)              colour            priority
//   surv        double flash  ##..##....  500 ms   orange, full       3 (highest)
//   join        double flash  ##..##....  500 ms   purple, full       2
//   new         single blink  ###...      300 ms   white, dimmer      1
//
// Rules:
//   - An active deauth always keeps the LED (the attack blink must stay visible while transmitting). Alerts that
//     arrive during a deauth are DROPPED, not queued: a blip minutes later would point at nothing.
//   - Rate limit + coalesce: at most one blip per kGapMs. A note that arrives inside the gap of a blip of the same or
//     higher priority is absorbed by it; a higher-priority note is kept pending and blips when the gap ends. Pending
//     kinds collapse to the highest one, so a burst of hits gives one blip (two at most when a surveillance hit
//     follows a lower-priority blip), never a strobe.
//   - Surveillance fires the first time this session a surv != 0 device gets a slot (Wi-Fi or BLE), independent of
//     the C4 event log. The radio paths only push a 32-bit MAC hash into a 4-entry ring under g_devMux; the loop
//     dedups against a 16-entry seen ring (a 17th distinct surveillance device may re-blip an old one - harmless).
//   - Permit-join: the 802.15.4 RX path is a true ISR, so it only sets g_ledJoinFlag when a node's permit-join bit
//     first becomes set (the bit is sticky per slot, so one node blips once, not at beacon rate).
//   - New: serviceEvents() (loop) calls ledAlertNote(LED_ALERT_NEW) for each "new" row, so it needs the log armed.
//
// Static RAM: ~100 B. Loop task only, except ledNoteSurv() (IRAM, caller holds g_devMux) and g_ledJoinFlag.
#include "bandwatch_core.h"

bool g_ledAlerts = true;
volatile bool g_ledJoinFlag = false;

namespace {
constexpr uint32_t kSlotMs = 50;
constexpr uint32_t kGapMs = 2000;
constexpr int kSurvRing = 4;      // radio -> loop hand-off
constexpr int kSurvSeen = 16;     // surveillance devices already blipped this session

struct BlipPattern { RgbColor c; uint8_t bright; uint8_t len; uint16_t mask; };   // bit n = slot n lit
constexpr BlipPattern kPatterns[] = {
    {{0, 0, 0}, 0, 0, 0},                        // NONE
    {{210, 210, 210}, 60, 6, 0x0007},            // NEW: white single 150 ms on, 150 ms off
    {{150, 0, 255}, 100, 10, 0x0033},            // JOIN: purple double flash
    {{255, 120, 0}, 100, 10, 0x0033},            // SURV: orange double flash
};

uint32_t survRing[kSurvRing];
volatile uint8_t survHead = 0, survTail = 0;
volatile bool survOverflow = false;
uint32_t survSeen[kSurvSeen];
uint8_t survSeenN = 0, survSeenAt = 0;

uint8_t pending = 0;          // bitmask of (1 << kind)
uint8_t blipKind = 0;         // drawing now (0 = idle)
uint8_t lastKind = 0;         // kind of the most recent blip, for coalescing inside its gap
int8_t lastLit = -1;          // last written on/off state, so the LED is written only on edges
uint32_t blipStartMs = 0;

bool survSeenAdd(uint32_t h) {   // true if new
    for (int i = 0; i < survSeenN; i++) if (survSeen[i] == h) return false;
    survSeen[survSeenAt] = h;
    survSeenAt = static_cast<uint8_t>((survSeenAt + 1) % kSurvSeen);
    if (survSeenN < kSurvSeen) survSeenN++;
    return true;
}

void note(LedAlertKind k, uint32_t now) {
    // Inside the gap of a blip that already said as much (same or higher priority): absorbed into it.
    if (lastKind && now - blipStartMs < kGapMs && k <= lastKind) return;
    pending |= static_cast<uint8_t>(1u << k);
}

void startBlip(LedAlertKind k, uint32_t now) {
    blipKind = k;
    lastKind = k;
    blipStartMs = now;
    lastLit = -1;
}
} // namespace

void IRAM_ATTR ledNoteSurv(const uint8_t* mac) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
    const uint8_t head = survHead, next = static_cast<uint8_t>((head + 1) % kSurvRing);
    if (next == survTail) { survOverflow = true; return; }
    survRing[head] = h;
    survHead = next;
}

void ledAlertNote(LedAlertKind k) {
    if (g_ledAlerts && k != LED_ALERT_NONE) note(k, millis());
}

void ledAlertTest(LedAlertKind k) {
    if (k == LED_ALERT_NONE || deauth.active) return;   // the deauth blink is never overridden, not even by a test
    pending = 0;
    startBlip(k, millis());
}

bool ledBlipActive() { return blipKind != 0; }

void serviceLedAlerts() {
    const uint32_t now = millis();
    for (;;) {
        uint32_t h = 0;
        portENTER_CRITICAL(&g_devMux);
        const bool have = survTail != survHead;
        if (have) { h = survRing[survTail]; survTail = static_cast<uint8_t>((survTail + 1) % kSurvRing); }
        portEXIT_CRITICAL(&g_devMux);
        if (!have) break;
        if (survSeenAdd(h) && g_ledAlerts) note(LED_ALERT_SURV, now);
    }
    if (survOverflow) { survOverflow = false; if (g_ledAlerts) note(LED_ALERT_SURV, now); }   // cannot dedup: blip anyway
    if (g_ledJoinFlag) { g_ledJoinFlag = false; if (g_ledAlerts) note(LED_ALERT_JOIN, now); }
    if (!g_ledAlerts) pending = 0;

    if (deauth.active) {   // the attack owns the LED: drop, do not queue
        pending = 0;
        blipKind = 0;
        return;
    }
    if (!blipKind && pending && now - blipStartMs >= kGapMs) {
        const LedAlertKind k = (pending & (1u << LED_ALERT_SURV)) ? LED_ALERT_SURV
                             : (pending & (1u << LED_ALERT_JOIN)) ? LED_ALERT_JOIN : LED_ALERT_NEW;
        pending = 0;
        startBlip(k, now);
    }
    if (!blipKind) return;
    const BlipPattern& p = kPatterns[blipKind];
    const uint32_t slot = (now - blipStartMs) / kSlotMs;
    if (slot >= p.len) { blipKind = 0; return; }   // driveLed() repaints the permanent state on its next tick
    const int8_t lit = (p.mask >> slot) & 1u;
    if (lit != lastLit) {
        lastLit = lit;
        if (lit) setLedColor(p.c, p.bright);
        else     rgbLedWrite(kRgbPin, 0, 0, 0);
    }
}
