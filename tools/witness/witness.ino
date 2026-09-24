// witness.ino - 2.4 GHz monitor-mode witness for the Bandwatch deauth investigation (docs/DEVELOPER.md §11).
//
// Runs on an ESP32-S3. RX only: it never associates, never scans, never transmits, so anything it
// reports came off the air from somewhere else. Point it at the channel the C5 is parked on and it
// tells you whether the C5's injected frames actually leave the antenna.
//
// Serial (115200): ch <n> | all 1|0 | stat | clear

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

static const char kTxTestSsid[] = "BANDWATCH-TXTEST";

enum Kind : uint8_t { K_DEAUTH = 0, K_DISASSOC, K_TXTEST, K_MGMT };
static const char* kKindName[] = { "DEAUTH", "DISASSOC", "TXTEST-BEACON", "MGMT" };

struct Evt {
  uint8_t  kind, sub;
  int8_t   rssi;
  uint8_t  ch;
  uint16_t seq, reason;
  uint32_t ms;
  uint8_t  a1[6], a2[6], a3[6];
};

static const size_t kRing = 96;
static Evt      ring[kRing];
static size_t   rHead = 0, rTail = 0;
static uint32_t nDropped = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t nMgmt, nData, nCtrl, nBeacon, nDeauth, nDisassoc, nTxtest;

static uint8_t chan     = 6;
static bool    printAll = false;

// Called on the WiFi task. Keep it short: copy out, never print here.
static void push(const Evt& e) {
  portENTER_CRITICAL(&mux);
  size_t next = (rHead + 1) % kRing;
  if (next == rTail) nDropped++;          // reader too slow; drop newest
  else { ring[rHead] = e; rHead = next; }
  portEXIT_CRITICAL(&mux);
}

static bool pop(Evt& out) {
  bool got = false;
  portENTER_CRITICAL(&mux);
  if (rTail != rHead) { out = ring[rTail]; rTail = (rTail + 1) % kRing; got = true; }
  portEXIT_CRITICAL(&mux);
  return got;
}

static void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (type == WIFI_PKT_DATA) { nData++; return; }
  if (type == WIFI_PKT_CTRL) { nCtrl++; return; }
  if (type != WIFI_PKT_MGMT) return;

  const uint8_t* f = p->payload;
  const int len = p->rx_ctrl.sig_len;
  if (len < 24) return;
  nMgmt++;

  const uint8_t sub = f[0] & 0xF0;
  Evt e{};
  e.sub    = sub;
  e.rssi   = p->rx_ctrl.rssi;
  e.ch     = p->rx_ctrl.channel;
  e.seq    = (uint16_t)((f[22] | (f[23] << 8)) >> 4);
  e.ms     = millis();
  memcpy(e.a1, f + 4,  6);
  memcpy(e.a2, f + 10, 6);
  memcpy(e.a3, f + 16, 6);

  if (sub == 0xC0) {                                   // deauthentication
    nDeauth++;
    e.kind   = K_DEAUTH;
    e.reason = (len >= 26) ? (uint16_t)(f[24] | (f[25] << 8)) : 0;
    push(e);
    return;
  }
  if (sub == 0xA0) {                                   // disassociation
    nDisassoc++;
    e.kind   = K_DISASSOC;
    e.reason = (len >= 26) ? (uint16_t)(f[24] | (f[25] << 8)) : 0;
    push(e);
    return;
  }
  if (sub == 0x80) {                                   // beacon
    nBeacon++;
    const uint8_t* t = f + 36;                         // skip fixed params (ts, interval, capab)
    const int tl = len - 36;
    if (tl >= 2 && t[0] == 0) {                        // SSID element
      const uint8_t sl = t[1];
      if (sl == sizeof(kTxTestSsid) - 1 && tl >= 2 + sl &&
          memcmp(t + 2, kTxTestSsid, sl) == 0) {
        nTxtest++;
        e.kind = K_TXTEST;
        push(e);
        return;
      }
    }
  }
  if (printAll) { e.kind = K_MGMT; push(e); }
}

static void macStr(const uint8_t* m, char* out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void setChannel(uint8_t c) {
  chan = c;
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
  Serial.printf("{\"t\":\"ch\",\"ch\":%u}\n", chan);
}

static void stats() {
  Serial.printf("{\"t\":\"stat\",\"ch\":%u,\"mgmt\":%lu,\"data\":%lu,\"ctrl\":%lu,"
                "\"beacon\":%lu,\"deauth\":%lu,\"disassoc\":%lu,\"txtest\":%lu,"
                "\"dropped\":%lu,\"up\":%lu}\n",
                chan, (unsigned long)nMgmt, (unsigned long)nData, (unsigned long)nCtrl,
                (unsigned long)nBeacon, (unsigned long)nDeauth, (unsigned long)nDisassoc,
                (unsigned long)nTxtest, (unsigned long)nDropped, (unsigned long)(millis() / 1000));
}

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);      // no association, no stored creds
  delay(100);
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);

  uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
  char ms[18]; macStr(mac, ms);
  Serial.printf("{\"t\":\"hello\",\"fw\":\"witness\",\"ver\":\"1.0\",\"chip\":\"esp32s3\","
                "\"mac\":\"%s\",\"ch\":%u,\"rx_only\":1}\n", ms, chan);
}

void loop() {
  // Serial commands
  static char line[64];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n) {
        if (!strncmp(line, "ch ", 3))        setChannel((uint8_t)atoi(line + 3));
        else if (!strncmp(line, "all ", 4))  { printAll = atoi(line + 4) != 0;
                                               Serial.printf("{\"t\":\"ack\",\"all\":%d}\n", printAll); }
        else if (!strcmp(line, "stat"))      stats();
        else if (!strcmp(line, "clear"))     { nMgmt = nData = nCtrl = nBeacon = nDeauth =
                                               nDisassoc = nTxtest = 0; nDropped = 0;
                                               Serial.println("{\"t\":\"ack\",\"clear\":1}"); }
        else Serial.printf("{\"t\":\"err\",\"cmd\":\"%s\"}\n", line);
      }
      n = 0;
    } else if (n < sizeof(line) - 1) line[n++] = c;
  }

  // Drain the ring
  Evt e;
  while (pop(e)) {
    char s1[18], s2[18], s3[18];
    macStr(e.a1, s1); macStr(e.a2, s2); macStr(e.a3, s3);
    Serial.printf("{\"t\":\"rx\",\"k\":\"%s\",\"sub\":\"0x%02x\",\"ms\":%lu,\"ch\":%u,\"rssi\":%d,"
                  "\"seq\":%u,\"reason\":%u,\"dst\":\"%s\",\"src\":\"%s\",\"bssid\":\"%s\"}\n",
                  kKindName[e.kind], e.sub, (unsigned long)e.ms, e.ch, e.rssi,
                  e.seq, e.reason, s1, s2, s3);
  }

  static uint32_t lastStat = 0;
  if (millis() - lastStat > 5000) { lastStat = millis(); stats(); }
}
