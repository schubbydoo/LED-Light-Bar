// Host-side tests for bridge_logic.h. No board needed:  ./run.sh
#include "../bridge_logic.h"
#include <cstdio>
#include <cstdlib>

using namespace bridge;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static const char* NAMES[] = {"bar", "jaw"};

static RouteResult R(const char* line, int& peer, const char*& payload) {
  const char* un; int unl;
  return route(line, NAMES, 2, peer, payload, un, unl);
}

static void test_routing() {
  int peer; const char* p;

  // COMPATIBILITY: a line with no prefix is for the bar and is passed through
  // untouched. Everything that worked before the jaw existed depends on this.
  CHECK(R("amb water", peer, p) == ROUTE_OK && peer == 0 && strcmp(p, "amb water") == 0);
  CHECK(R("cue 12 0 40", peer, p) == ROUTE_OK && peer == 0 && strcmp(p, "cue 12 0 40") == 0);
  CHECK(R("trk d 3 AAAA", peer, p) == ROUTE_OK && peer == 0 && strcmp(p, "trk d 3 AAAA") == 0);

  CHECK(R("@jaw cue 12 0 40", peer, p) == ROUTE_OK && peer == 1 && strcmp(p, "cue 12 0 40") == 0);
  CHECK(R("@bar st", peer, p) == ROUTE_OK && peer == 0 && strcmp(p, "st") == 0);
  CHECK(R("@JAW st", peer, p) == ROUTE_OK && peer == 1);            // case-insensitive
  CHECK(R("@jaw    st", peer, p) == ROUTE_OK && strcmp(p, "st") == 0); // extra spaces
  CHECK(R("@jaw\tst", peer, p) == ROUTE_OK && peer == 1);

  // Only the first token is the address; later '@' is ordinary payload.
  CHECK(R("@jaw say @bar", peer, p) == ROUTE_OK && peer == 1 && strcmp(p, "say @bar") == 0);

  const char* un; int unl;
  CHECK(route("@jawx st", NAMES, 2, peer, p, un, unl) == ROUTE_UNKNOWN_PEER);
  CHECK(unl == 4 && strncmp(un, "jawx", 4) == 0);
  CHECK(R("@ st", peer, p) == ROUTE_UNKNOWN_PEER);   // empty name
  CHECK(R("@", peer, p) == ROUTE_UNKNOWN_PEER);
  CHECK(R("@jaw", peer, p) == ROUTE_EMPTY);          // nothing to send
  CHECK(R("@jaw   ", peer, p) == ROUTE_EMPTY);
}

static void test_trim() {
  char a[] = "  st \r\n"; trim(a); CHECK(strcmp(a, "st") == 0);
  char b[] = "st";        trim(b); CHECK(strcmp(b, "st") == 0);
  char c[] = "   ";       trim(c); CHECK(strcmp(c, "") == 0);
  char d[] = "";          trim(d); CHECK(strcmp(d, "") == 0);
  char e[] = "\tset a b"; trim(e); CHECK(strcmp(e, "set a b") == 0);
}

static void test_gap() {
  CHECK(isBulk("trk begin 1 100"));
  CHECK(isBulk("trk d 4 AAAA"));
  CHECK(isBulk("trk end"));
  CHECK(!isBulk("cue 1 0 0"));
  CHECK(!isBulk("trkx"));
  CHECK(gapAfter("cue 1 0 0", 60) == 60);
  CHECK(gapAfter("cue 1 0 0", 10) == 10);          // tuned down: allowed
  CHECK(gapAfter("trk d 1 AA", 10) == GAP_BULK_MS); // ...but never for tracks
  CHECK(gapAfter("trk d 1 AA", 0) == GAP_BULK_MS);
}

static void test_clock_wrap() {
  CHECK(reached(100, 100));
  CHECK(reached(101, 100));
  CHECK(!reached(99, 100));
  // millis() wraps at 2^32. A gap that straddles the wrap must still work.
  uint32_t t = 0xFFFFFFF0u + 60;            // wraps to 44
  CHECK(!reached(0xFFFFFFF0u, t));
  CHECK(!reached(0xFFFFFFFFu, t));
  CHECK(!reached(43, t));
  CHECK(reached(44, t));
  CHECK(reached(100, t));
}

static void test_queue_order_and_pacing() {
  Queue q;
  uint32_t next[MAX_PEERS] = {0, 0, 0, 0};
  Slot s;

  // Bar's chunks queue up; the jaw's cue arrives behind them.
  CHECK(q.push(0, "trk d 1 A"));
  CHECK(q.push(0, "trk d 2 B"));
  CHECK(q.push(1, "cue 5 0 0"));
  CHECK(q.push(0, "trk end"));

  // Everything is allowed at t=1000: strict FIFO.
  int i = q.pick(1000, next);
  CHECK(i == 0);
  q.take(i, s);
  CHECK(strcmp(s.text, "trk d 1 A") == 0 && s.peer == 0);
  next[0] = 1000 + 60;     // the bar is now inside its gap

  // THE POINT OF THE CHANGE: the jaw's cue goes next, without waiting for the
  // bar's gap — it was behind two bar lines in the queue.
  i = q.pick(1010, next);
  CHECK(i >= 0);
  q.take(i, s);
  CHECK(s.peer == 1 && strcmp(s.text, "cue 5 0 0") == 0);
  next[1] = 1010 + 60;

  // Nothing else is allowed until the bar's gap ends.
  CHECK(q.pick(1030, next) == -1);
  CHECK(q.pick(1059, next) == -1);

  // And the bar's lines then go out IN ORDER: `trk end` never overtakes `trk d 2`.
  i = q.pick(1060, next); CHECK(i == 0);
  q.take(i, s); CHECK(strcmp(s.text, "trk d 2 B") == 0);
  next[0] = 1060 + 60;
  CHECK(q.pick(1100, next) == -1);
  i = q.pick(1120, next); CHECK(i == 0);
  q.take(i, s); CHECK(strcmp(s.text, "trk end") == 0);
  CHECK(q.empty());
}

static void test_queue_limits() {
  Queue q;
  for (int i = 0; i < QUEUE_SLOTS; i++) CHECK(q.push(0, "x"));
  CHECK(q.full());
  CHECK(!q.push(0, "overflow"));       // refused, not silently dropped later
  char big[MAX_LINE + 2];
  memset(big, 'a', sizeof(big)); big[MAX_LINE + 1] = 0;
  Queue q2;
  CHECK(!q2.push(0, big));             // 241 characters: too long
  big[MAX_LINE] = 0;
  CHECK(q2.push(0, big));              // exactly 240: fine
}

int main() {
  test_routing(); test_trim(); test_gap(); test_clock_wrap();
  test_queue_order_and_pacing(); test_queue_limits();
  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("all bridge logic tests passed\n");
  return 0;
}
