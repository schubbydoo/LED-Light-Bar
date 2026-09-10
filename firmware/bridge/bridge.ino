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

// The light bar. See firmware/README.md — that table is the authority, and each
// address appears exactly once per file that needs it.
static uint8_t PEER_BAR[6] = {0xF0, 0x9E, 0x9E, 0xB2, 0xA8, 0x28};

// Fixed, and identical on the bar. Nothing negotiates this for us.
static const uint8_t LINK_CHANNEL = 1;

static volatile uint32_t sentOk = 0, sentFail = 0;
static volatile bool lastFailed = false;

// New-style callbacks. This core (esp32 3.3.x / IDF 5.5) passes an info struct
// rather than a bare MAC, and the two-argument `(const uint8_t*, status)` form
// every tutorial still shows does not compile against it.
static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) sentOk++;
  else { sentFail++; lastFailed = true; }
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  // Whatever the bar says comes straight out on serial, prefixed so it cannot be
  // mistaken for something this board said.
  Serial.print(F("bar> "));
  for (int i = 0; i < len; i++) Serial.write(data[i]);
  Serial.println();
}

static bool linkSend(const String& s) {
  esp_err_t e = esp_now_send(PEER_BAR, (const uint8_t*)s.c_str(), s.length());
  return e == ESP_OK;
}

void setup() {
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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                 // never associate; we only want the radio
  esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println();
  Serial.println(F("=== SFX Box bridge — serial <-> ESP-NOW =================="));
  Serial.print  (F("my MAC   : ")); Serial.println(WiFi.macAddress());
  Serial.print  (F("peer     : "));
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (PEER_BAR[i] < 16) Serial.print('0');
    Serial.print(PEER_BAR[i], HEX);
  }
  Serial.println();
  Serial.print  (F("channel  : ")); Serial.println(LINK_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW INIT FAILED — nothing will send."));
    return;
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PEER_BAR, 6);
  peer.channel = LINK_CHANNEL;       // 0 would mean "whatever channel we are on"
  peer.encrypt = false;              // AES is available; not while bringing up
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println(F("could not add peer"));
    return;
  }

  Serial.println(F("ready. Anything you type is forwarded to the bar verbatim."));
  Serial.println(F("  try:  amb water   |   set yellow 0.9   |   speak 4"));
  Serial.println(F("  '.stats' reports delivery counts (local, not forwarded)"));
  Serial.println(F("=========================================================="));
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      // 240, not 200. A track chunk is "trk d NN " plus 200 base64 characters —
      // 210 total — and a 200-char cap silently truncated every one of them by
      // 8 characters, i.e. 6 bytes of envelope. It did not error: the line
      // forwarded fine, just short. The receiver's sequence check is what
      // caught it, because a shortfall per chunk would otherwise have shifted
      // the whole track progressively earlier and read as bad sync.
      //
      // ESP_NOW_MAX_DATA_LEN is 250, so this is the real ceiling; anything
      // longer must be split by the sender, not quietly clipped here.
      if (buf.length() < 240) buf += c;
      continue;
    }
    buf.trim();
    if (!buf.length()) { buf = ""; continue; }

    // Commands for the bridge itself are prefixed with a dot, so they cannot
    // collide with anything the bar might ever understand.
    if (buf == ".stats") {
      Serial.print(F("sent ok ")); Serial.print(sentOk);
      Serial.print(F("  failed ")); Serial.println(sentFail);
    } else if (buf == ".mac") {
      Serial.println(WiFi.macAddress());
    } else {
      lastFailed = false;
      bool queued = linkSend(buf);
      // The callback lands a moment later. Wait briefly so the reply that gets
      // printed belongs to the line just sent — this is a bring-up tool, and
      // knowing *which* send failed matters more than throughput.
      uint32_t t = millis();
      while (millis() - t < 60) { delay(1); }
      if (!queued)          Serial.println(F("-> send refused (peer not added?)"));
      else if (lastFailed)  Serial.println(F("-> DELIVERY FAILED — bar off, out "
                                             "of range, or on another channel"));
      else                  Serial.println(F("-> ok"));
    }
    buf = "";
  }
}
