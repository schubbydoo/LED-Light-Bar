// Drives the REAL bridge.ino on a PC, against stand-ins for the radio and the
// serial port (test/stubs/). Run with ./run.sh.
//
// This checks the behaviour that matters and cannot be seen by reading: what
// went to WHOM, in what ORDER, and WHEN — including that a line for one prop
// no longer waits behind another's, and that the bar's replies are unchanged.
//
// It does NOT prove the sketch compiles against the real ESP32 core. See
// stubs/Arduino.h.

#include "Arduino.h"

uint32_t g_now = 1000;
FakeSerial Serial;
FakeWiFi WiFi;
std::vector<SentPacket> g_sent;
esp_now_send_cb_t g_sendCb = nullptr;
esp_now_recv_cb_t g_recvCb = nullptr;
std::vector<std::vector<uint8_t>> g_peersAdded;

#include "../bridge.ino"     // the sketch under test, unmodified

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL line %d: %s\n--- serial output was:\n%s---\n", __LINE__, #cond, Serial.out.c_str()); \
  failures++; } } while (0)

static bool has(const char* s) { return Serial.out.find(s) != std::string::npos; }

// Run `ms` milliseconds. The fake radio reports each send's outcome one
// millisecond after it was made, as the real one does within a few.
enum Ack { ACK_OK, ACK_FAIL, ACK_NONE };
static size_t acked = 0;
static void pump(int ms, Ack mode = ACK_OK) {
  for (int t = 0; t < ms; t++) {
    loop();                                    // includes delay(1)
    while (acked < g_sent.size()) {
      acked++;
      if (mode == ACK_NONE) continue;
      esp_now_send_info_t info = {};
      g_sendCb(&info, mode == ACK_OK ? ESP_NOW_SEND_SUCCESS : ESP_NOW_SEND_FAIL);
    }
  }
}
static void fresh() {                          // quiet the bridge between scenarios
  pump(300);
  g_sent.clear(); acked = 0; Serial.out.clear();
}
static bool toMac(const SentPacket& p, int peer) { return memcmp(p.mac, PEERS[peer].mac, 6) == 0; }

static void scenario_boot() {
  // The shipped table now has the jaw's real MAC. These scenarios start from an
  // UNSET prop, to keep covering the refusal path, so blank it before boot.
  memset(PEERS[1].mac, 0, 6);
  setup();
  CHECK(has("jaw  (MAC not set)"));
  CHECK(has("* bar  F0:9E:9E:B2:A8:28"));
  CHECK(g_peersAdded.size() == 1);             // only the configured prop is registered
}

static void scenario_default_goes_to_bar_unchanged() {
  fresh();
  Serial.feed("amb water\n");
  pump(100);
  CHECK(g_sent.size() == 1);
  CHECK(g_sent[0].data == "amb water");
  CHECK(toMac(g_sent[0], 0));
  // The bar's reply is exactly what the old bridge printed — no suffix.
  CHECK(Serial.out == "-> ok\n");
}

static void scenario_unset_prop_refused() {
  fresh();
  Serial.feed("@jaw st\n");
  pump(100);
  CHECK(g_sent.empty());
  CHECK(has("@jaw has no MAC set"));
}

static void scenario_unknown_and_empty() {
  fresh();
  Serial.feed("@owl st\n@bar\n");
  pump(100);
  CHECK(g_sent.empty());
  CHECK(has("unknown prop @owl"));
  CHECK(has("known: bar jaw"));
  CHECK(has("nothing to send after the @name"));
}

static void scenario_one_prop_does_not_wait_for_another() {
  // The jaw has now been built.
  const uint8_t jaw[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  memcpy(PEERS[1].mac, jaw, 6);

  fresh();
  // Bar's track chunks, and a cue for the jaw queued BEHIND them.
  Serial.feed("trk d 1 A\n@jaw cue 5 0 0\ntrk d 2 B\ntrk end\n");
  pump(400);

  CHECK(g_sent.size() == 4);
  if (g_sent.size() == 4) {
    CHECK(g_sent[0].data == "trk d 1 A" && toMac(g_sent[0], 0));
    // THE POINT: the jaw's cue goes out at once, not 60 ms later behind the bar.
    CHECK(g_sent[1].data == "cue 5 0 0" && toMac(g_sent[1], 1));   // prefix stripped
    CHECK(g_sent[1].at - g_sent[0].at <= 5);
    // The bar's lines stay in order and keep the proven 60 ms spacing.
    CHECK(g_sent[2].data == "trk d 2 B" && toMac(g_sent[2], 0));
    CHECK(g_sent[3].data == "trk end"   && toMac(g_sent[3], 0));
    CHECK(g_sent[2].at - g_sent[0].at >= 60);
    CHECK(g_sent[3].at - g_sent[2].at >= 60);
  }
  CHECK(has("-> ok @jaw"));                    // the jaw's reply is tagged...
  CHECK(has("-> ok\n"));                       // ...the bar's is not
}

static void scenario_bar_alone_keeps_60ms() {
  fresh();
  Serial.feed("st\nst\nst\n");
  pump(400);
  CHECK(g_sent.size() == 3);
  if (g_sent.size() == 3) {
    CHECK(g_sent[1].at - g_sent[0].at >= 60);
    CHECK(g_sent[2].at - g_sent[1].at >= 60);
  }
}

static void scenario_gap_tuning() {
  fresh();
  Serial.feed(".gap 10\n");
  pump(5);
  CHECK(has("-> gap 10 ms (trk lines stay at 60)"));

  Serial.out.clear();
  Serial.feed("st\nst\n");
  pump(100);
  CHECK(g_sent.size() == 2);
  if (g_sent.size() == 2) {
    CHECK(g_sent[1].at - g_sent[0].at >= 10);
    CHECK(g_sent[1].at - g_sent[0].at < 60);   // actually faster
  }

  fresh();
  Serial.feed("trk d 1 A\ntrk d 2 B\n");        // bulk is never hurried
  pump(300);
  CHECK(g_sent.size() == 2);
  if (g_sent.size() == 2) CHECK(g_sent[1].at - g_sent[0].at >= 60);

  fresh();
  Serial.feed(".gap 5000\n.gap -3\n.gap 60\n");
  pump(10);
  CHECK(has("gap must be 0..1000 ms"));
  CHECK(has("-> gap 60 ms"));
}

static void scenario_failures_are_reported_per_prop() {
  fresh();
  Serial.feed("@jaw st\n");
  pump(100, ACK_FAIL);
  CHECK(has("-> DELIVERY FAILED @jaw"));

  fresh();
  Serial.feed("st\n");
  pump(100, ACK_FAIL);
  CHECK(has("-> DELIVERY FAILED — prop off"));      // the bar: no @tag
  CHECK(!has("@bar"));
}

static void scenario_missing_callback_does_not_wedge() {
  fresh();
  Serial.feed("st\n");
  pump(250, ACK_NONE);
  CHECK(has("NO SEND CALLBACK"));
  // ...and the bridge recovers: the next line still goes out.
  Serial.out.clear(); g_sent.clear(); acked = 0;
  Serial.feed("st\n");
  pump(200, ACK_OK);
  CHECK(g_sent.size() == 1);
  CHECK(has("-> ok"));
}

static void scenario_long_lines_refused_not_clipped() {
  fresh();
  std::string ok240(240, 'a');
  Serial.feed(ok240 + "\n");
  pump(100);
  CHECK(g_sent.size() == 1 && g_sent[0].data.size() == 240);     // exactly the cap: fine

  fresh();
  Serial.feed(std::string(241, 'a') + "\n");
  pump(100);
  CHECK(g_sent.empty());                                         // not sent as a stump
  CHECK(has("line too long (241 > 240)"));

  fresh();
  Serial.feed("@jaw " + ok240 + "\n");                           // prefix does not count
  pump(100);
  CHECK(g_sent.size() == 1 && g_sent[0].data.size() == 240 && toMac(g_sent[0], 1));

  fresh();
  Serial.feed(std::string(400, 'a') + "\n");                     // overflows the buffer
  pump(100);
  CHECK(g_sent.empty());
  CHECK(has("line too long"));
}

static void scenario_queue_full_is_loud() {
  fresh();
  std::string burst;
  for (int i = 0; i < 20; i++) burst += "st\n";
  Serial.feed(burst);
  loop();                                    // takes all 20 before anything can drain
  int full = 0;
  for (size_t p = Serial.out.find("queue full"); p != std::string::npos;
       p = Serial.out.find("queue full", p + 1)) full++;
  CHECK(full == 4);                          // 16 slots; the other 4 refused, not lost silently
  pump(2000);                                // drain so later scenarios start clean
}

static void scenario_inbound_is_tagged_by_sender() {
  fresh();
  const char msg[] = "ack st";
  uint8_t barMac[6]; memcpy(barMac, PEERS[0].mac, 6);
  uint8_t jawMac[6]; memcpy(jawMac, PEERS[1].mac, 6);
  uint8_t other[6] = {1, 2, 3, 4, 5, 6};

  esp_now_recv_info_t ri = {};
  ri.src_addr = barMac; g_recvCb(&ri, (const uint8_t*)msg, 6);
  ri.src_addr = jawMac; g_recvCb(&ri, (const uint8_t*)msg, 6);
  ri.src_addr = other;  g_recvCb(&ri, (const uint8_t*)msg, 6);
  CHECK(Serial.out == "bar> ack st\njaw> ack st\nbar> ack st\n");   // unknown -> treated as before
}

static void scenario_local_commands() {
  fresh();
  // The counters are cumulative across every earlier scenario, so check the
  // change this one makes and that the report matches the counters exactly.
  uint32_t bar0 = sentOk[0], jaw0 = sentOk[1];
  Serial.feed("st\n@jaw st\n");
  pump(200);
  CHECK(sentOk[0] == bar0 + 1 && sentOk[1] == jaw0 + 1);
  Serial.out.clear();
  Serial.feed(".stats\n.peers\n.bogus\n");
  pump(5);
  uint32_t ok = sentOk[0] + sentOk[1], fail = sentFail[0] + sentFail[1];
  CHECK(has(("sent ok " + std::to_string(ok) + "  failed " + std::to_string(fail) + "\n").c_str()));
  CHECK(has(("  bar ok " + std::to_string(sentOk[0]) + "  failed " + std::to_string(sentFail[0])).c_str()));
  CHECK(has(("  jaw ok " + std::to_string(sentOk[1]) + "  failed " + std::to_string(sentFail[1])).c_str()));
  CHECK(has("* bar  F0:9E:9E:B2:A8:28"));
  CHECK(has("AA:BB:CC:DD:EE:01"));
  CHECK(has("unknown bridge command"));
}

int main() {
  scenario_boot();
  scenario_default_goes_to_bar_unchanged();
  scenario_unset_prop_refused();
  scenario_unknown_and_empty();
  scenario_one_prop_does_not_wait_for_another();
  scenario_bar_alone_keeps_60ms();
  scenario_gap_tuning();
  scenario_failures_are_reported_per_prop();
  scenario_missing_callback_does_not_wedge();
  scenario_long_lines_refused_not_clipped();
  scenario_queue_full_is_loud();
  scenario_inbound_is_tagged_by_sender();
  scenario_local_commands();
  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("all bridge sketch scenarios passed\n");
  return 0;
}
