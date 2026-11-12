/*
  Bridge logic that needs no hardware: which prop a line is for, and the queue
  that decides when each line may go out.

  WHY THIS IS A SEPARATE FILE
  There is no way to run an ESP32 in the place this was written, but plain C++
  runs anywhere. Keeping the decisions here — routing, ordering, pacing — means
  they are tested by `test/run.sh` before a board is ever flashed, and the
  sketch is left holding only the parts that genuinely need the radio. The
  Arduino build compiles this file as-is; the test compiles the very same file.

  Nothing in here may include Arduino.h or touch a pin, the radio or a clock.
  Time is always passed in.
*/
#pragma once

#include <stdint.h>
#include <string.h>

namespace bridge {

// Longest payload that goes over the air. ESP_NOW_MAX_DATA_LEN is 250; a track
// chunk is "trk d NN " + 200 base64 characters = ~210, so 240 has headroom.
constexpr int MAX_LINE    = 240;
constexpr int MAX_PEERS   = 4;
constexpr int QUEUE_SLOTS = 16;

// ---------------------------------------------------------------------------
// Routing.   "@jaw cue 12 0 40"  ->  peer jaw, payload "cue 12 0 40"
//            "cue 12 0 40"       ->  peer 0 (the bar), payload unchanged
//
// A line with no prefix goes to peer 0, and its payload is the line untouched.
// That is the compatibility rule: everything that worked before this existed —
// the box, send.sh, a person typing at a terminal — addresses the bar by saying
// nothing, and must keep working byte for byte.

enum RouteResult { ROUTE_OK = 0, ROUTE_UNKNOWN_PEER = 1, ROUTE_EMPTY = 2 };

inline bool sameName(const char* a, int aLen, const char* b) {
  if ((int)strlen(b) != aLen) return false;
  for (int i = 0; i < aLen; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return false;
  }
  return true;
}

// `names[0]` is the default peer. On ROUTE_UNKNOWN_PEER, `unknownName` points at
// the name that failed (not terminated — use `unknownLen`).
inline RouteResult route(const char* line, const char* const* names, int nNames,
                         int& peer, const char*& payload,
                         const char*& unknownName, int& unknownLen) {
  peer = 0;
  payload = line;
  unknownName = nullptr;
  unknownLen = 0;
  if (line[0] != '@') return ROUTE_OK;

  const char* name = line + 1;
  int n = 0;
  while (name[n] && name[n] != ' ' && name[n] != '\t') n++;

  int found = -1;
  for (int i = 0; i < nNames; i++) {
    if (sameName(name, n, names[i])) { found = i; break; }
  }
  if (found < 0) {
    unknownName = name;
    unknownLen = n;
    return ROUTE_UNKNOWN_PEER;
  }

  const char* rest = name + n;
  while (*rest == ' ' || *rest == '\t') rest++;
  if (!*rest) return ROUTE_EMPTY;        // "@jaw" and nothing to say to it

  peer = found;
  payload = rest;
  return ROUTE_OK;
}

// Strip leading and trailing whitespace in place.
inline void trim(char* s) {
  int n = (int)strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
  int lead = 0;
  while (s[lead] == ' ' || s[lead] == '\t') lead++;
  if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

// ---------------------------------------------------------------------------
// Pacing.
//
// Two facts from the bar's own firmware decide what is safe here:
//   * ESP-NOW sends are asynchronous. A second queued before the first has
//     completed loses BOTH. So: exactly one send in flight, ever.
//   * The bar holds ONE received line and drops anything that arrives before
//     its loop has taken it. So: back-to-back lines to the SAME peer need a gap.
//
// The old bridge handled both by sleeping 60 ms after every line, which blocked
// everything — a cue for one prop waited behind a position update for another.
// The gap belongs to the RECEIVER, so it is tracked per peer: a line for the jaw
// may go out while the bar is still inside its gap.
//
// 60 ms is the value the bar was proven against, so it stays the default until
// someone MEASURES that less is safe. Track chunks always keep it: that is the
// one operation that must not lose a line, and nothing is gained by hurrying it.

constexpr uint32_t GAP_BULK_MS    = 60;   // "trk ..." lines — never tuned away
constexpr uint32_t GAP_DEFAULT_MS = 60;   // everything else; `.gap` changes it

inline bool isBulk(const char* text) {
  return strncmp(text, "trk ", 4) == 0 || strcmp(text, "trk") == 0;
}

// How long the RECEIVER is left alone after this line. Bulk lines always get the
// proven gap, even if `normalGapMs` has been tuned down.
inline uint32_t gapAfter(const char* text, uint32_t normalGapMs) {
  return isBulk(text) ? GAP_BULK_MS : normalGapMs;
}

// Has `now` reached `t`? Correct across the 49-day millis() wrap.
inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// ---------------------------------------------------------------------------
// The queue. A small FIFO that can be asked "what is the first line I am
// allowed to send right now?" — which is not always the head, because the head
// may belong to a peer that is still inside its gap.
//
// ORDER IS KEPT PER PEER. A later line is only chosen over an earlier one when
// the two are for different peers, so `trk end` can never overtake `trk d`, and
// a cue can never overtake the track it refers to.

struct Slot {
  int      peer;
  uint16_t len;
  char     text[MAX_LINE + 1];
};

struct Queue {
  Slot     slot[QUEUE_SLOTS];
  int      count = 0;

  bool full()  const { return count >= QUEUE_SLOTS; }
  bool empty() const { return count == 0; }

  bool push(int peer, const char* text) {
    if (full()) return false;
    int n = (int)strlen(text);
    if (n > MAX_LINE) return false;
    Slot& s = slot[count++];
    s.peer = peer;
    s.len = (uint16_t)n;
    memcpy(s.text, text, n + 1);
    return true;
  }

  // Index of the first slot whose peer may send at `now`, or -1.
  // `nextAllowed[p]` is the time peer p's gap ends.
  int pick(uint32_t now, const uint32_t* nextAllowed) const {
    for (int i = 0; i < count; i++) {
      if (reached(now, nextAllowed[slot[i].peer])) return i;
    }
    return -1;
  }

  // Remove slot i, keeping the rest in order.
  void take(int i, Slot& out) {
    out = slot[i];
    for (int j = i; j < count - 1; j++) slot[j] = slot[j + 1];
    count--;
  }
};

}  // namespace bridge
