/*
  The bridge — the SFX Box's radio.

  A XIAO ESP32C3 on the Pi's USB that turns serial lines into ESP-NOW packets
  and back. The Pi 4 cannot transmit ESP-NOW on its own radio: the protocol
  rides 802.11 vendor-specific action frames and needs monitor mode with packet
  injection, which the onboard CYW43455 does not do. Hence a co-processor.

  ESP-NOW rather than WiFi, and the distinction is the whole reason:
  it uses the 2.4 GHz radio and *nothing else from the stack* — no access point,
  no SSID, no association, no DHCP, no router. Two ESP32s address each other by
  MAC. The link comes up when both ends have power and stays up with every
  router in the building switched off. The tiki already runs its eyes on 433 MHz
  relays and needs no network; putting the light bar on WiFi would make the whole
  prop depend on infrastructure it currently does without.

  ---------------------------------------------------------------------------
  IT FORWARDS TEXT, VERBATIM

  Anything typed at this board's serial port goes out as-is, and the bar feeds it
  into the SAME command parser it already uses for its own serial port. So every
  command the bar understands worked over the radio the moment this existed —
  `amb water`, `set yellow 0.9`, `speak 4` — with no protocol to keep in step.

  That is worth more than a compact binary format at this stage. The wire is
  human-readable, so a mismatch between the two ends is *visible* rather than a
  silently misaligned struct, and the link can be exercised by hand before
  anything on the Pi knows it exists. Control traffic is a handful of small
  packets per show; there is nothing to optimise.

  The one thing that will NOT be text is the envelope track, which is bulk data
  pushed at provisioning time. That earns a binary message type when it lands.

  ---------------------------------------------------------------------------
  MORE THAN ONE PROP

  A line is for the light bar unless it says otherwise:

      amb water            -> the bar          (unchanged: nothing needs to change)
      @bar amb water       -> the bar, said explicitly
      @jaw cue 12 0 40     -> the jaw prop

  The prefix is stripped; the prop never sees it, so every prop understands the
  same command language and a new prop needs no change to the protocol.

  Replies come back tagged by who sent them. The bar's are exactly what they
  always were (`bar> ...` and `-> ok`); any other prop gets its name on them
  (`jaw> ...`, `-> ok @jaw`) so two props talking at once cannot be confused.

  PACING IS PER PROP AND DOES NOT BLOCK. This used to sleep 60 ms after every
  line, which stalled the whole bridge — with two props, a cue for one would
  wait behind a position update for the other, in a system whose sync is
  measured in tens of milliseconds. Now each prop has its own gap, and a line
  for one prop can go out while another is inside its gap. The gap itself is
  UNCHANGED at 60 ms, because that is what the bar was proven against and
  nobody has measured less. `.gap <ms>` tunes it live for normal lines; `trk`
  lines always keep 60. The logic is in bridge_logic.h and is tested without a
  board by test/run.sh.

  ---------------------------------------------------------------------------
  THE PART THAT IS NOT NEGOTIABLE

  Both ends must be on the same WiFi channel. Neither is associated with an
  access point, so nothing negotiates it for them — it is set explicitly at both
  ends and must match. A channel mismatch does not error: `esp_now_send` returns
  ESP_OK and the delivery callback reports failure, which reads as "the other
  board is off" and sends you looking at power.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "bridge_logic.h"

// ---------------------------------------------------------------------------
// The props this bridge can talk to. See firmware/README.md — that table is the
// authority for MACs, and each address appears exactly once per file that needs
// it. Entry 0 is the DEFAULT: a line with no `@name` goes there, which is what
// keeps everything written before a second prop existed working untouched.
//
// An all-zero MAC means "this prop has not been built yet". It is skipped when
// peers are registered, and a line addressed to it is refused with a clear
// message rather than sent into the air at nobody.
struct Peer {
  const char* name;
  uint8_t     mac[6];
};

static Peer PEERS[] = {
  { "bar", {0xF0, 0x9E, 0x9E, 0xB2, 0xA8, 0x28} },   // the light bar
  { "jaw", {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },   // NOT SET — fill in when the board arrives
};
static const int N_PEERS = sizeof(PEERS) / sizeof(PEERS[0]);
static_assert(sizeof(PEERS) / sizeof(PEERS[0]) <= bridge::MAX_PEERS, "raise MAX_PEERS");

static const char* PEER_NAMES[bridge::MAX_PEERS];

static bool peerConfigured(int i) {
  for (int k = 0; k < 6; k++) if (PEERS[i].mac[k]) return true;
  return false;
}

static int peerByMac(const uint8_t* mac) {
  for (int i = 0; i < N_PEERS; i++) {
    if (peerConfigured(i) && memcmp(PEERS[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

// Fixed, and identical on every prop. Nothing negotiates this for us.
static const uint8_t LINK_CHANNEL = 1;

// ---------------------------------------------------------------------------
// State. One send is in flight at a time, ever — see bridge_logic.h for why.

static bridge::Queue  queue;
static uint32_t       nextAllowed[bridge::MAX_PEERS];   // end of each prop's gap
static uint32_t       gapMs = bridge::GAP_DEFAULT_MS;   // `.gap` changes this

static bool           inFlight = false;
static int            flightPeer = 0;
static char           flightText[bridge::MAX_LINE + 1];
static uint32_t       flightSince = 0;
static const uint32_t FLIGHT_TIMEOUT_MS = 200;   // the callback normally lands in a few

// Set by the radio callback, read by loop(). The callback runs in the WiFi task
// and must not do anything slow, so it only records the outcome.
static volatile bool  sendDone = false;
static volatile bool  sendOk = false;

static uint32_t sentOk[bridge::MAX_PEERS], sentFail[bridge::MAX_PEERS];

// New-style callbacks. This core (esp32 3.3.x / IDF 5.5) passes an info struct
// rather than a bare MAC, and the two-argument `(const uint8_t*, status)` form
// every tutorial still shows does not compile against it.
static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  sendOk = (status == ESP_NOW_SEND_SUCCESS);
  sendDone = true;
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  // Whatever a prop says comes straight out on serial, prefixed so it cannot be
  // mistaken for something this board said. The bar's prefix is exactly what it
  // has always been; another prop is named, so two talking at once stay apart.
  int who = peerByMac(info->src_addr);
  if (who <= 0) Serial.print(F("bar> "));          // the bar — and anything unrecognised
  else { Serial.print(PEERS[who].name); Serial.print(F("> ")); }
  for (int i = 0; i < len; i++) Serial.write(data[i]);
  Serial.println();
}

// What a reply to the sender looks like. The bar's must stay byte-identical to
// the old bridge: the SFX Box may be matching on "-> ok". Other props add a
// suffix, which a prefix match still accepts.
static void reply(int peer, const __FlashStringHelper* what) {
  Serial.print(F("-> ")); Serial.print(what);
  if (peer != 0) { Serial.print(F(" @")); Serial.print(PEERS[peer].name); }
  Serial.println();
}

static void replyFailed(int peer) {
  Serial.print(F("-> DELIVERY FAILED"));
  if (peer != 0) { Serial.print(F(" @")); Serial.print(PEERS[peer].name); }
  Serial.println(F(" — prop off, out of range, or on another channel"));
}

// ---------------------------------------------------------------------------
// Sending. loop() calls this every pass; it never waits.

static void finishFlight(bool ok, bool noCallback) {
  uint32_t now = millis();
  if (ok) { sentOk[flightPeer]++; reply(flightPeer, F("ok")); }
  else {
    sentFail[flightPeer]++;
    if (noCallback) {
      Serial.println(F("-> NO SEND CALLBACK — radio did not report; treating as failed"));
    } else {
      replyFailed(flightPeer);
    }
  }
  // The gap runs from when the send COMPLETED, not from when it was queued.
  nextAllowed[flightPeer] = now + bridge::gapAfter(flightText, gapMs);
  inFlight = false;
}

static void serviceSending() {
  uint32_t now = millis();

  if (inFlight) {
    if (sendDone)                                      finishFlight(sendOk, false);
    else if (now - flightSince > FLIGHT_TIMEOUT_MS)    finishFlight(false, true);
    else return;                                       // still on its way
  }

  int i = queue.pick(now, nextAllowed);
  if (i < 0) return;

  bridge::Slot s;
  queue.take(i, s);
  flightPeer = s.peer;
  strcpy(flightText, s.text);

  sendDone = false;                                    // clear BEFORE sending
  esp_err_t e = esp_now_send(PEERS[s.peer].mac, (const uint8_t*)s.text, s.len);
  if (e != ESP_OK) {
    sentFail[s.peer]++;
    Serial.print(F("-> send refused"));
    if (s.peer != 0) { Serial.print(F(" @")); Serial.print(PEERS[s.peer].name); }
    Serial.println(F(" (peer not added?)"));
    nextAllowed[s.peer] = millis() + bridge::gapAfter(s.text, gapMs);
    return;
  }
  inFlight = true;
  flightSince = millis();
}

// ---------------------------------------------------------------------------
// One complete line from the host.

static void printMac(const uint8_t* m) {
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (m[i] < 16) Serial.print('0');
    Serial.print(m[i], HEX);
  }
}

static void printPeerTable() {
  for (int i = 0; i < N_PEERS; i++) {
    Serial.print(i == 0 ? F("  * ") : F("    "));      // * marks the default
    Serial.print(PEERS[i].name); Serial.print(F("  "));
    if (peerConfigured(i)) printMac(PEERS[i].mac);
    else Serial.print(F("(MAC not set)"));
    Serial.println();
  }
}

static void handleLine(char* line) {
  bridge::trim(line);
  if (!*line) return;

  // Commands for the bridge itself are prefixed with a dot, so they cannot
  // collide with anything a prop might ever understand.
  if (line[0] == '.') {
    if (!strcmp(line, ".stats")) {
      // First line keeps its old format, in case something reads it.
      uint32_t ok = 0, fail = 0;
      for (int i = 0; i < N_PEERS; i++) { ok += sentOk[i]; fail += sentFail[i]; }
      Serial.print(F("sent ok ")); Serial.print(ok);
      Serial.print(F("  failed ")); Serial.println(fail);
      for (int i = 0; i < N_PEERS; i++) {
        Serial.print(F("  ")); Serial.print(PEERS[i].name);
        Serial.print(F(" ok ")); Serial.print(sentOk[i]);
        Serial.print(F("  failed ")); Serial.println(sentFail[i]);
      }
    } else if (!strcmp(line, ".mac")) {
      Serial.println(WiFi.macAddress());
    } else if (!strcmp(line, ".peers")) {
      printPeerTable();
    } else if (!strncmp(line, ".gap", 4)) {
      // `.gap` reports; `.gap 30` sets. Never touches trk pacing.
      if (line[4] == ' ' && line[5]) {
        long v = atol(line + 5);
        if (v < 0 || v > 1000) Serial.println(F("-> gap must be 0..1000 ms"));
        else { gapMs = (uint32_t)v; Serial.print(F("-> gap ")); Serial.print(gapMs);
               Serial.println(F(" ms (trk lines stay at 60)")); }
      } else {
        Serial.print(F("gap ")); Serial.print(gapMs);
        Serial.println(F(" ms (trk lines stay at 60)"));
      }
    } else {
      Serial.println(F("-> unknown bridge command. Known: .stats .mac .peers .gap"));
    }
    return;
  }

  int peer;
  const char* payload;
  const char* badName; int badLen;
  bridge::RouteResult r = bridge::route(line, PEER_NAMES, N_PEERS, peer, payload,
                                        badName, badLen);
  if (r == bridge::ROUTE_UNKNOWN_PEER) {
    Serial.print(F("-> unknown prop @"));
    for (int i = 0; i < badLen; i++) Serial.write(badName[i]);
    Serial.print(F(" — known:"));
    for (int i = 0; i < N_PEERS; i++) { Serial.print(' '); Serial.print(PEERS[i].name); }
    Serial.println();
    return;
  }
  if (r == bridge::ROUTE_EMPTY) {
    Serial.println(F("-> nothing to send after the @name"));
    return;
  }
  if (!peerConfigured(peer)) {
    Serial.print(F("-> @")); Serial.print(PEERS[peer].name);
    Serial.println(F(" has no MAC set — edit PEERS in bridge.ino"));
    return;
  }
  // Refuse, loudly. The old code clipped an over-long line at 240 characters and
  // sent the stump — a track chunk arriving 8 characters short corrupted the
  // track without any error. A line that cannot be sent whole is not sent.
  if (strlen(payload) > (size_t)bridge::MAX_LINE) {
    Serial.print(F("-> line too long (")); Serial.print(strlen(payload));
    Serial.print(F(" > ")); Serial.print(bridge::MAX_LINE);
    Serial.println(F("), not sent — split it at the sender"));
    return;
  }
  if (!queue.push(peer, payload)) {
    Serial.println(F("-> bridge queue full, line dropped"));
    return;
  }
  // The reply ("-> ok" / "-> DELIVERY FAILED") is printed when the radio reports
  // back, from serviceSending(). It is no longer printed before this returns.
}

// ---------------------------------------------------------------------------

void setup() {
  // 80 MHz, not the default 160. This board only shuttles text between serial
  // and the radio; it was running warm for no benefit. The radio is unaffected
  // (TX power and channel are untouched), so link range does not change.
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  // NEVER BLOCK ON A PORT NOBODY IS READING.
  //
  // This is USB CDC, not a UART. If no host has the endpoint open, the TX
  // buffer fills and Serial.println() waits for a reader that will never come —
  // so the sketch stalls, stops servicing the radio, and looks like a link
  // fault. It cost a long diagnosis: every track push SUCCEEDED while a serial
  // monitor was open and FAILED without one, because the monitor was draining
  // the buffer. A prop in a tiki has nothing attached.
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }

  for (int i = 0; i < N_PEERS; i++) PEER_NAMES[i] = PEERS[i].name;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                 // never associate; we only want the radio
  esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println();
  Serial.println(F("=== SFX Box bridge — serial <-> ESP-NOW =================="));
  Serial.print  (F("my MAC   : ")); Serial.println(WiFi.macAddress());
  Serial.print  (F("channel  : ")); Serial.println(LINK_CHANNEL);
  Serial.println(F("props    : (* = default, gets lines with no @name)"));
  printPeerTable();

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW INIT FAILED — nothing will send."));
    return;
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  for (int i = 0; i < N_PEERS; i++) {
    if (!peerConfigured(i)) continue;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEERS[i].mac, 6);
    peer.channel = LINK_CHANNEL;     // 0 would mean "whatever channel we are on"
    peer.encrypt = false;            // AES is available; not while bringing up
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.print(F("could not add peer ")); Serial.println(PEERS[i].name);
    }
  }

  Serial.println(F("ready. Anything you type goes to the bar; '@jaw ...' goes to the jaw."));
  Serial.println(F("  try:  amb water   |   set yellow 0.9   |   speak 4"));
  Serial.println(F("  '.stats' delivery counts   '.peers' the table   '.gap' pacing  (local)"));
  Serial.println(F("=========================================================="));
}

void loop() {
  // Collect a line without ever waiting for the rest of it.
  static char buf[bridge::MAX_LINE + 16];
  static int  len = 0;
  static bool overflow = false;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      // Room for "@name " in front of a full-size payload. A line that still
      // does not fit is not clipped and sent — it is refused whole, below.
      if (len < (int)sizeof(buf) - 1) buf[len++] = c;
      else overflow = true;
      continue;
    }
    if (overflow) {
      Serial.println(F("-> line too long, not sent — split it at the sender"));
    } else if (len) {
      buf[len] = 0;
      handleLine(buf);
    }
    len = 0;
    overflow = false;
  }

  serviceSending();

  // Yield between polls. Without this loop() spins flat out: the Arduino core's
  // loop task outranks the idle task, so the CPU never idles and the board runs
  // warm around the clock. 1 ms costs nothing now that nothing waits on it.
  delay(1);
}
