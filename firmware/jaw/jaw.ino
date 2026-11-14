/*
  Jaw — a servo mouth that speaks the way the light bar's fire does.

  This is the bar's remote with the strip swapped for a servo. The link, the track,
  the cue, the persistence and the command language are the same as `bar.ino`, on
  purpose: the SFX Box and its web UI talk to this board exactly as they talk to the
  bar, and only the renderer differs. See docs/06-jaw-plan.md.

  THE RULE THAT CARRIES OVER, UNCHANGED

      angle = closed + dir * swing * shape(env)

  At env == 0 that is exactly `closed`. There is no talking mode, no crossfade and
  no second renderer: idle is speech with a zero envelope. The box sends no angles,
  ever — only the two scalars (level, onset) in the track, and the jaw turns the
  level into an angle.

  WHAT DRIVES IT

  The level channel of the same gated envelope the fire uses. The onset channel is
  carried in the track but ignored here. If the jaw reads as mushy, the fix is a
  differently-shaped track on the box side, not speech knowledge in this file.

  ---------------------------------------------------------------------------
  EVERYTHING BELOW IS UNTESTED ON A REAL JAW

  The defaults are starting points for tuning by eye, not measurements. What HAS
  been measured, on the bench servo with no load (docs/07-servo-s51.md): the S51
  follows a 3.3 V signal from D10, holds at ~3 mA, peaks ~61 mA moving freely and
  ~459 mA stalled. Nothing has been tried against a real jaw or a real track.

  SETTINGS (`set <key> <value>`; `save` keeps them across a power cycle)

    closedDeg    rest / mouth-shut angle                     0..180   default 90
    swingDeg     how far the mouth opens from closed         0..90    default 20
    dir          +1: opening raises the angle, -1: lowers it          default +1
    loDeg/hiDeg  HARD limits — the output is never outside   0..180   60 / 120
    gain         how much of the swing a full-level word uses 0..2    default 1.0
    deadband     level below this does not move the jaw      0..0.9   default 0.08
    curve        >1 opens late and wide, <1 opens early      0.3..3   default 1.0
    attack       smoothing coefficient, opening              0.01..1  default 0.6
    release      smoothing coefficient, closing              0.01..1  default 0.25
    slewDegS     the most the servo is ASKED to move per s   30..1000 default 400
    lookaheadMs  read the track this far ahead (servo lag)   0..300   default 0
    idleDetachS  stop the pulse after this long at rest      0..600   default 0 (never)

  idleDetachS defaults to 0 on purpose. Holding costs ~3 mA, so detaching buys
  nothing, and a detached jaw on a borrowed prop may simply fall open.

  ---------------------------------------------------------------------------
  CALIBRATION, WITH THE JAW MOUNTED

    hold <deg>   pin the jaw at an angle (clamped to loDeg..hiDeg). `idle` releases.
    open / close hold the open / closed angle.
    Widen loDeg / hiDeg a few degrees at a time, watching a power meter. A servo
    driven past a mechanical stop draws its stall current until told to move.

  ---------------------------------------------------------------------------
  SEVERAL SERVOS ON ONE BOARD (docs/06-jaw-plan.md §10)

  One board is one prop; each servo is a CHANNEL (1..4, `channels <n>`, default 1).
  Channel 1 is on D10, 2..4 on D1, D2, D3 (GPIO3/4/5 — never D0, D8, D9). Every
  command without a channel means channel 1, and the single-servo replies are
  unchanged, so a box that knows only one servo keeps working.

  A channel's MODE is firmware state:  mode <ch> speech|manual
    speech  follows the box's track exactly as the single jaw always did
    manual  goes to a named POSITION SLOT on `go <ch> <slot>`, eased at the slot's
            speed; optionally `auto <ch> on` repeats slot autoA <-> autoB by itself.
  Names and labels live on the box; the wire carries channel and slot NUMBERS only.

    set <ch>.<key> <v>          one parameter on a channel (no prefix = channel 1)
    slot <ch> <n> <deg> <speed> define slot n (1..8): angle and travel deg/s
    go <ch> <n>                 go to slot n; ends that channel's `auto`
    auto <ch> on|off            repeat autoA<->autoB (autoGapS, autoJitter, autoHoldS)
    stop | stop all             release the track | ALSO end every channel's `auto`

  `auto on` is a DEAD-MAN: it expires after 120 s unless repeated. It is RAM-only
  and off at boot, so a box that crashes mid-show cannot leave a servo moving.

  Pins: D10 (GPIO10), D1..D3. Never D0, D8 or D9 — see CLAUDE.md, strapping pins.
*/

#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <stddef.h>

// Declared up front: the Arduino build inserts function prototypes above the
// struct definitions, and several of them take a Chan or a Knob.
struct Chan;
struct Knob;

static const int      MAX_CH    = 4;
static const int      MAX_SLOTS = 8;
static const int      CH_PINS[MAX_CH] = {D10, D1, D2, D3};     // GPIO10, 3, 4, 5
static const uint32_t PWM_HZ    = 50;
static const uint8_t  PWM_BITS  = 14;                      // 16384 counts per 20 ms
static const uint32_t PERIOD_US = 1000000UL / PWM_HZ;

// Datasheet mapping (docs/07-servo-s51.md): 500 us = 0 deg, 2500 us = 180 deg.
// A pulse outside this range is never produced, whatever is typed.
static const float US_MIN = 500.0f, US_MAX = 2500.0f;

static const uint8_t  FPS      = 50;                       // the servo's frame rate AND the track's
static const uint32_t FRAME_US = 1000000UL / FPS;

static const uint32_t AUTO_DEADMAN_MS = 120000;            // `auto on` lapses unless repeated

// ---------------------------------------------------------------------------
// The link — copied from bar.ino, which is the reference. Text lines over
// ESP-NOW go into the same handleLine() the serial port uses.
// ---------------------------------------------------------------------------
static uint8_t PEER_BRIDGE[6] = {0xF0, 0x9E, 0x9E, 0xB2, 0x3D, 0x38};
static const uint8_t LINK_CHANNEL = 1;
static bool linkUp = false;

static void handleLine(String line);

// One reply per received line: ESP-NOW sends are asynchronous, and a second
// queued before the first completes loses BOTH. See bar.ino.
//
// `st` and `show` now answer with several lines (a header, then one per channel).
// They go through a small queue that sends the next only after the previous send
// has COMPLETED, so the one-in-flight rule still holds.
static bool linkReplied = false;
static void linkQuiet() { linkReplied = true; }

static const int TXQ = 10;
static String   txq[TXQ];
static int      txHead = 0, txCount = 0;
static volatile bool txBusy = false;
static uint32_t txAt = 0;
static const uint32_t TX_GAP_MS = 25;

static void linkSend(const String& s) {
  if (!linkUp) return;
  linkReplied = true;
  if (txCount >= TXQ) return;                              // drop, never block
  txq[(txHead + txCount) % TXQ] = s;
  txCount++;
}

static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  txBusy = false;
}

static void pumpTx() {
  if (!txCount) return;
  uint32_t now = millis();
  if (txBusy && now - txAt > 150) txBusy = false;          // a callback that never came
  if (txBusy || now - txAt < TX_GAP_MS) return;
  String& s = txq[txHead];
  txBusy = true;
  txAt = now;
  esp_now_send(PEER_BRIDGE, (const uint8_t*)s.c_str(), s.length());
  s = "";
  txHead = (txHead + 1) % TXQ;
  txCount--;
}

static volatile bool  rxPending = false;
static char           rxBuf[250];
static volatile int   rxLen = 0;

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (rxPending) return;
  if (len > (int)sizeof(rxBuf) - 1) len = sizeof(rxBuf) - 1;
  memcpy(rxBuf, data, len);
  rxBuf[len] = 0;
  rxLen = len;
  rxPending = true;
}

static void linkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW init FAILED — serial still works"));
    return;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PEER_BRIDGE, 6);
  peer.channel = LINK_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println(F("could not add bridge as peer"));
    return;
  }
  linkUp = true;
}

// ---------------------------------------------------------------------------
// Parameters. All floats, so a table drives `show`, `set` and `get` and adding a
// knob is one line. Two structs per channel, stored separately on purpose:
//
//   Params  the original single-servo knobs. Channel 1's blob keeps the SAME NVS
//           key, version and size it always had, so a board already tuned keeps
//           its settings across this firmware.
//   Extra   what several servos added: mode, the auto-move settings, the slots.
// ---------------------------------------------------------------------------
struct Params {
  float closedDeg   = 90.0f;
  float swingDeg    = 20.0f;
  float dir         = 1.0f;
  float loDeg       = 60.0f;
  float hiDeg       = 120.0f;
  float gain        = 1.0f;
  float deadband    = 0.08f;
  float curve       = 1.0f;
  float attack      = 0.6f;
  float release     = 0.25f;
  float slewDegS    = 400.0f;
  float lookaheadMs = 0.0f;
  float idleDetachS = 0.0f;
};

struct Extra {
  float mode        = 0.0f;       // 0 speech, 1 manual — set with `mode`, not `set`
  float autoA       = 1.0f;       // slot the auto-move rests at
  float autoB       = 2.0f;       // slot it visits (a blink's "closed")
  float autoGapS    = 4.0f;       // average rest between visits
  float autoJitter  = 0.5f;       // 0..1, +/- fraction of the gap
  float autoHoldS   = 0.15f;      // time spent at B before returning
  float slotDeg[MAX_SLOTS];       // < 0: not defined
  float slotSpeed[MAX_SLOTS];     // deg/s
  Extra() { for (int i = 0; i < MAX_SLOTS; i++) { slotDeg[i] = -1.0f; slotSpeed[i] = 200.0f; } }
};

struct Chan {
  Params p;
  Extra  x;
  // runtime
  float    env = 0.0f;            // smoothed level, 0..1 (speech)
  float    holdDeg = -1.0f;       // >=0: pinned at an angle (`hold`, `open`, `close`)
  float    manualTarget = -1.0f;  // >=0: where a manual channel is going
  float    moveSpeed = 200.0f;    // deg/s toward manualTarget
  int      curSlot = 0;           // last slot commanded, 0 = none yet
  float    angle = 90.0f;         // what is being commanded now, after slew
  bool     attached = false;
  bool     limited = false;
  uint32_t restSince = 0;
  bool     autoOn = false;
  uint32_t autoUntil = 0, autoNext = 0, phaseStart = 0, holdStart = 0;
  uint8_t  autoPhase = 0;         // 0 resting at A, 1 going to / holding at B, 2 returning
};

static Chan C[MAX_CH];
static Chan DEF[MAX_CH];            // compiled defaults, for `forget`
static int  nch = 1;                // channels in use

struct Knob { const char* name; uint8_t which; uint16_t off; float lo, hi; };
#define PK(n, lo, hi) {#n, 0, (uint16_t)offsetof(Params, n), lo, hi}
#define XK(n, lo, hi) {#n, 1, (uint16_t)offsetof(Extra, n), lo, hi}
static const Knob KNOBS[] = {
  PK(closedDeg,   0.0f,  180.0f),
  PK(swingDeg,    0.0f,   90.0f),
  PK(dir,        -1.0f,    1.0f),
  PK(loDeg,       0.0f,  180.0f),
  PK(hiDeg,       0.0f,  180.0f),
  PK(gain,        0.0f,    2.0f),
  PK(deadband,    0.0f,    0.9f),
  PK(curve,       0.3f,    3.0f),
  PK(attack,      0.01f,   1.0f),
  PK(release,     0.01f,   1.0f),
  PK(slewDegS,   30.0f,  1000.0f),
  PK(lookaheadMs, 0.0f,  300.0f),
  PK(idleDetachS, 0.0f,  600.0f),
  XK(autoA,       1.0f,    (float)MAX_SLOTS),
  XK(autoB,       1.0f,    (float)MAX_SLOTS),
  XK(autoGapS,    0.5f,  120.0f),
  XK(autoJitter,  0.0f,    1.0f),
  XK(autoHoldS,   0.0f,   10.0f),
};
static const int N_KNOBS = sizeof(KNOBS) / sizeof(KNOBS[0]);
static const int N_PARAM_KNOBS = 13;       // the first 13 are Params; the rest are Extra

static float* knobPtr(Chan& c, const Knob& k) {
  return (float*)((char*)(k.which ? (void*)&c.x : (void*)&c.p) + k.off);
}

static Preferences prefs;
static const char*    NVS_NS         = "jaw";
static const uint16_t PARAMS_VERSION = 1;
static const uint16_t EXTRA_VERSION  = 1;

static void saveParams() {
  prefs.begin(NVS_NS, false);
  prefs.putUShort("pver", PARAMS_VERSION);
  prefs.putUShort("psize", (uint16_t)sizeof(Params));
  prefs.putBytes("params", &C[0].p, sizeof(Params));            // channel 1: unchanged key
  prefs.putUShort("xver", EXTRA_VERSION);
  prefs.putUShort("nch", (uint16_t)nch);
  for (int i = 0; i < MAX_CH; i++) {
    if (i > 0) prefs.putBytes((String("p") + (i + 1)).c_str(), &C[i].p, sizeof(Params));
    prefs.putBytes((String("x") + (i + 1)).c_str(), &C[i].x, sizeof(Extra));
  }
  prefs.end();
}

static void loadParams() {
  prefs.begin(NVS_NS, true);
  uint16_t ver  = prefs.getUShort("pver", 0);
  uint16_t size = prefs.getUShort("psize", 0);
  if (ver == PARAMS_VERSION && size == sizeof(Params)) {
    Params tmp;
    if (prefs.getBytes("params", &tmp, sizeof(tmp)) == sizeof(tmp)) C[0].p = tmp;
  } else if (ver) {
    Serial.printf("params: stored v%u/%uB ignored (this build wants v%u/%uB)\n",
                  ver, size, PARAMS_VERSION, (unsigned)sizeof(Params));
  }
  if (prefs.getUShort("xver", 0) == EXTRA_VERSION) {
    int n = prefs.getUShort("nch", 1);
    if (n >= 1 && n <= MAX_CH) nch = n;
    for (int i = 0; i < MAX_CH; i++) {
      Params p; Extra x;
      if (i > 0 && prefs.getBytes((String("p") + (i + 1)).c_str(), &p, sizeof(p)) == sizeof(p)) C[i].p = p;
      if (prefs.getBytes((String("x") + (i + 1)).c_str(), &x, sizeof(x)) == sizeof(x)) C[i].x = x;
    }
  }
  prefs.end();
}

static void clearParams() {
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  nch = 1;
  for (int i = 0; i < MAX_CH; i++) { C[i].p = DEF[i].p; C[i].x = DEF[i].x; }
}

// 0 = ok, else a reason. Kept apart from the clamp so "no such parameter" keeps
// meaning exactly one thing, the way the bar's does.
static bool setParam(Chan& c, const String& k, float v, String& note) {
  for (int i = 0; i < N_KNOBS; i++) {
    if (k != KNOBS[i].name) continue;
    v = constrain(v, KNOBS[i].lo, KNOBS[i].hi);
    if (k == "dir") v = (v < 0.0f) ? -1.0f : 1.0f;
    if (k == "autoA" || k == "autoB") v = roundf(v);
    float oldLo = c.p.loDeg, oldHi = c.p.hiDeg;
    *knobPtr(c, KNOBS[i]) = v;
    if (c.p.loDeg + 5.0f > c.p.hiDeg) {        // the window must stay a window
      c.p.loDeg = oldLo; c.p.hiDeg = oldHi;
      note = "refused: need loDeg + 5 <= hiDeg";
      return false;
    }
    return true;
  }
  note = "no such parameter: " + k;
  return false;
}

// ---------------------------------------------------------------------------
// The track, cue and position — copied from bar.ino. The renderer is ignorant of
// transport; it steps whatever track it is handed. One track serves every speech
// channel.
// ---------------------------------------------------------------------------
static const uint16_t TRACK_MAX_FRAMES = 8000;             // 160 s at 50 Hz
static const uint8_t  FRAME_MS = 20;
static const uint32_t TRACK_CHUNK_BYTES = 150;             // must match the sender

static uint8_t  trackBuf[TRACK_MAX_FRAMES * 2];            // level, onset per frame
static uint16_t trackFrames = 0;
static uint16_t trackClip = 0;
static uint16_t rxFrames = 0;
static uint32_t rxBytes = 0;

static const char*    TRACK_PATH  = "/track.bin";
static const uint32_t TRACK_MAGIC = 0x4B52544A;            // "JTRK"
static const uint16_t TRACK_VER   = 1;
struct TrackHeader { uint32_t magic; uint16_t version; uint16_t clip; uint16_t frames; };

static bool fsReady = false;

static bool saveTrack() {
  if (!fsReady || !trackFrames) return false;
  File f = LittleFS.open(TRACK_PATH, "w");
  if (!f) return false;
  TrackHeader h{TRACK_MAGIC, TRACK_VER, trackClip, trackFrames};
  bool ok = f.write((uint8_t*)&h, sizeof(h)) == sizeof(h);
  if (ok) { size_t n = (size_t)trackFrames * 2; ok = f.write(trackBuf, n) == n; }
  f.close();
  if (!ok) LittleFS.remove(TRACK_PATH);                    // never leave half a track
  return ok;
}

static bool loadTrack() {
  if (!fsReady || !LittleFS.exists(TRACK_PATH)) return false;
  File f = LittleFS.open(TRACK_PATH, "r");
  if (!f) return false;
  TrackHeader h{};
  bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)
            && h.magic == TRACK_MAGIC && h.version == TRACK_VER
            && h.frames > 0 && h.frames <= TRACK_MAX_FRAMES;
  if (ok) {
    size_t n = (size_t)h.frames * 2;
    ok = f.read(trackBuf, n) == n;
    if (ok) { trackFrames = h.frames; trackClip = h.clip; }
  }
  f.close();
  return ok;
}

static bool     playing = false;
static uint32_t cueLocalMs = 0;
static int32_t  cueTrackMs = 0;

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static uint32_t b64decode(const char* in, uint8_t* out, uint32_t outMax) {
  uint32_t n = 0;
  int quad[4], q = 0;
  for (const char* p = in; *p; p++) {
    int v = b64val(*p);
    if (v < 0) continue;
    quad[q++] = v;
    if (q < 4) continue;
    q = 0;
    uint32_t triple = (quad[0] << 18) | (quad[1] << 12) | (quad[2] << 6) | quad[3];
    if (n < outMax) out[n++] = (triple >> 16) & 0xFF;
    if (n < outMax) out[n++] = (triple >> 8) & 0xFF;
    if (n < outMax) out[n++] = triple & 0xFF;
  }
  // The trailing partial group is not optional — see bar.ino. A chunk that pads
  // arrives short without it, on a transport that otherwise looks proven.
  if (q == 2) {
    if (n < outMax) out[n++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
  } else if (q == 3) {
    if (n < outMax) out[n++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    if (n < outMax) out[n++] = (uint8_t)((quad[1] << 4) | (quad[2] >> 2));
  }
  return n;
}

static int32_t trackPosMs() { return (int32_t)(millis() - cueLocalMs) + cueTrackMs; }

// ---------------------------------------------------------------------------
// The renderer.
// ---------------------------------------------------------------------------
static float rawLevel = 0.0f;       // the level every speech channel is fed this frame
static float trackLevel = 0.0f;     // same, for `st`
static float envHold = -1.0f;       // >=0: a manual level hold (`env`)
static uint32_t speakUntil = 0, speakStart = 0;
static bool  speechActive = false;

static float clampDeg(const Chan& c, float d) {
  float lo = fmaxf(0.0f, c.p.loDeg), hi = fminf(180.0f, c.p.hiDeg);
  if (d < lo) d = lo;
  if (d > hi) d = hi;
  return d;
}

static float degToUs(float d) {
  float us = US_MIN + d * (US_MAX - US_MIN) / 180.0f;
  return constrain(us, US_MIN, US_MAX);
}

static void writeAngle(int i, float d) {
  Chan& c = C[i];
  if (!c.attached) { ledcAttach(CH_PINS[i], PWM_HZ, PWM_BITS); c.attached = true; }
  uint32_t duty = (uint32_t)(((double)degToUs(d) * ((1u << PWM_BITS) - 1)) / PERIOD_US);
  ledcWrite(CH_PINS[i], duty);
}

static void detachServo(int i) {
  Chan& c = C[i];
  if (!c.attached) return;
  ledcDetach(CH_PINS[i]);
  pinMode(CH_PINS[i], OUTPUT);
  digitalWrite(CH_PINS[i], LOW);
  c.attached = false;
}

static float shape(const Chan& c, float e) {
  float x = (e - c.p.deadband) / fmaxf(0.01f, 1.0f - c.p.deadband);
  if (x < 0.0f) x = 0.0f;
  x = powf(fminf(x, 1.0f), c.p.curve) * c.p.gain;
  return fminf(x, 1.0f);
}

// The track (or a hold, or the stand-in) becomes ONE level per frame; each speech
// channel then smooths and shapes it with its own knobs.
static void feedSpeech(float dt) {
  float raw = 0.0f;
  trackLevel = 0.0f;
  float lookahead = C[0].p.lookaheadMs;
  if (envHold >= 0.0f) {
    raw = envHold;
    trackLevel = envHold;
  } else if (playing) {
    int32_t pos = trackPosMs() + (int32_t)lookahead;
    if (pos >= 0) {
      uint32_t f = (uint32_t)pos / FRAME_MS;
      if (f >= trackFrames) {
        playing = false;
        Serial.println(F("track: finished"));
      } else {
        raw = trackBuf[f * 2] / 255.0f;
        trackLevel = raw;
      }
    }
  } else if (speakUntil && millis() < speakUntil) {
    // A stand-in: syllables at ~4 Hz inside phrases, a gap every couple of
    // seconds, so attack, release and swing can be judged before a track exists.
    float t = (millis() - speakStart) / 1000.0f;
    if (fmodf(t, 2.6f) < 1.9f) {
      float syl = 0.5f + 0.5f * sinf(t * 2.0f * PI * 4.0f);
      raw = powf(syl, 1.6f) * (0.55f + 0.45f * sinf(t * 1.7f));
      if (raw < 0.0f) raw = 0.0f;
      trackLevel = raw;
    }
  } else if (speakUntil && millis() >= speakUntil) {
    speakUntil = 0;
    Serial.println(F("speak: done"));
  }

  speechActive = playing || (envHold >= 0.0f) || (speakUntil && millis() < speakUntil);
  rawLevel = raw;
}

static float slotDegOf(const Chan& c, int slot) {          // slot is 1-based; < 0 = not defined
  if (slot < 1 || slot > MAX_SLOTS) return -1.0f;
  return c.x.slotDeg[slot - 1];
}

static bool reached(const Chan& c) {
  float t = (c.manualTarget >= 0.0f) ? c.manualTarget : c.p.closedDeg;
  return fabsf(c.angle - clampDeg(c, t)) < 0.5f;
}

static void goSlot(Chan& c, int slot) {
  c.manualTarget = c.x.slotDeg[slot - 1];
  c.moveSpeed = c.x.slotSpeed[slot - 1];
  c.curSlot = slot;
}

// Move on its own: rest at autoA, visit autoB, come back, wait a jittered gap.
// A phase that cannot reach its target (a slot beyond the limits) times out
// instead of waiting forever.
static void autoTick(Chan& c) {
  if (!c.autoOn) return;
  uint32_t now = millis();
  if ((int32_t)(now - c.autoUntil) >= 0) {
    c.autoOn = false;
    int a = (int)c.x.autoA;
    if (slotDegOf(c, a) >= 0.0f) goSlot(c, a);
    Serial.println(F("auto: lapsed (not repeated) — stopped"));
    return;
  }
  int a = (int)c.x.autoA, b = (int)c.x.autoB;
  if (c.autoPhase == 0) {
    if ((int32_t)(now - c.autoNext) >= 0) {
      goSlot(c, b); c.autoPhase = 1; c.phaseStart = now; c.holdStart = 0;
    }
  } else if (c.autoPhase == 1) {
    if (!c.holdStart && (reached(c) || now - c.phaseStart > 3000)) c.holdStart = now;
    if (c.holdStart && now - c.holdStart >= (uint32_t)(c.x.autoHoldS * 1000.0f)) {
      goSlot(c, a); c.autoPhase = 2; c.phaseStart = now;
    }
  } else {
    if (reached(c) || now - c.phaseStart > 3000) {
      float r = esp_random() / 4294967295.0f;
      float gap = c.x.autoGapS * (1.0f + c.x.autoJitter * (2.0f * r - 1.0f));
      c.autoNext = now + (uint32_t)fmaxf(200.0f, gap * 1000.0f);
      c.autoPhase = 0;
    }
  }
}

static void render(int i, float dt) {
  Chan& c = C[i];
  autoTick(c);

  float target;
  float speed = c.p.slewDegS;
  bool speech = c.x.mode < 0.5f;

  if (speech) {
    float k = (rawLevel > c.env) ? c.p.attack : c.p.release;
    k = 1.0f - powf(1.0f - k, dt * FPS);
    c.env += (rawLevel - c.env) * k;
    if (c.env < 0.002f) c.env = 0.0f;
  } else {
    c.env = 0.0f;
  }

  if (c.holdDeg >= 0.0f) {
    target = c.holdDeg;
  } else if (speech) {
    target = c.p.closedDeg + c.p.dir * c.p.swingDeg * shape(c, c.env);
  } else if (c.manualTarget >= 0.0f) {
    target = c.manualTarget;
    speed = c.moveSpeed;
  } else {
    target = c.p.closedDeg;
  }
  float clamped = clampDeg(c, target);
  c.limited = fabsf(clamped - target) > 0.01f;
  target = clamped;

  // The most the servo is ASKED to move. A real S51 cannot follow a raw 50 Hz
  // envelope, and asking it to is how a servo chatters and wears.
  float maxStep = speed * dt;
  float d = target - c.angle;
  if (d >  maxStep) d =  maxStep;
  if (d < -maxStep) d = -maxStep;
  c.angle += d;

  bool atRest = speech
      ? (fabsf(c.angle - clampDeg(c, c.p.closedDeg)) < 0.05f && !speechActive && c.holdDeg < 0.0f)
      : (fabsf(c.angle - target) < 0.05f && !c.autoOn && c.holdDeg < 0.0f);
  if (!atRest) c.restSince = millis();
  if (c.p.idleDetachS > 0.0f && atRest && millis() - c.restSince > (uint32_t)(c.p.idleDetachS * 1000.0f)) {
    detachServo(i);
    return;
  }
  writeAngle(i, c.angle);
}

// ---------------------------------------------------------------------------
// Commands.
// ---------------------------------------------------------------------------

static void reply(const String& m) { Serial.println(m); linkSend(m); }

static String knobValue(Chan& c, int i) {
  char b[16];
  snprintf(b, sizeof(b), "%g", *knobPtr(c, KNOBS[i]));
  return String(b);
}

static String modeName(const Chan& c) { return c.x.mode < 0.5f ? "speech" : "manual"; }

static String posOf(const Chan& c) {
  if (c.x.mode < 0.5f) return "-";
  if (c.curSlot < 1) return "home";
  float d = slotDegOf(c, c.curSlot);
  return (d >= 0.0f && fabsf(c.angle - clampDeg(c, d)) < 0.5f) ? String(c.curSlot) : String("between");
}

static String slotsLine(int i) {
  Chan& c = C[i];
  String out = "slots ch=" + String(i + 1) + " mode=" + modeName(c);
  for (int k = N_PARAM_KNOBS; k < N_KNOBS; k++) out += String(" ") + KNOBS[k].name + "=" + knobValue(c, k);
  for (int s = 0; s < MAX_SLOTS; s++)
    if (c.x.slotDeg[s] >= 0.0f) out += " " + String(s + 1) + "=" + String(c.x.slotDeg[s], 0) + "/" + String(c.x.slotSpeed[s], 0);
  return out;
}

static void showParams() {
  for (int i = 0; i < nch; i++) {
    if (nch > 1) Serial.printf("-- channel %d (%s) --\n", i + 1, modeName(C[i]).c_str());
    for (int k = 0; k < N_KNOBS; k++) {
      Serial.print(KNOBS[k].name);
      for (int pad = strlen(KNOBS[k].name); pad < 13; pad++) Serial.print(' ');
      Serial.println(knobValue(C[i], k));
    }
    Serial.println(slotsLine(i));
  }
  Serial.print(F("env (live)   ")); Serial.println(C[0].env, 3);
}

// "2" -> 1 (a zero-based channel); returns -1 for anything that is not 1..nch.
static int chArg(const String& s) {
  String t = s; t.trim();
  if (!t.length() || t[0] < '1' || t[0] > '9') return -1;
  int n = t.toInt();
  return (n >= 1 && n <= nch) ? n - 1 : -1;
}

static String noChannel(const String& s) {
  return "refused: no channel " + s + " (channels=" + String(nch) + ")";
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""   : line.substring(sp + 1);
  rest.trim();

  // `show` prints one knob per line on serial and answers over the radio in a few
  // lines of key=value pairs, so the web UI can read every setting without opening
  // the board's serial port (which reboots it). The FIRST line is channel 1's and
  // has no `ch=` — exactly what a single-servo box has always read. Then, per
  // extra channel, `show ch=N ...`; then `slots ch=N ...` for every channel.
  if (verb == "show" || verb == "get") {
    showParams();
    String out = "show";
    for (int k = 0; k < N_PARAM_KNOBS; k++) out += String(" ") + KNOBS[k].name + "=" + knobValue(C[0], k);
    linkSend(out);
    for (int i = 1; i < nch; i++) {
      String o = "show ch=" + String(i + 1);
      for (int k = 0; k < N_PARAM_KNOBS; k++) o += String(" ") + KNOBS[k].name + "=" + knobValue(C[i], k);
      linkSend(o);
    }
    for (int i = 0; i < nch; i++) linkSend(slotsLine(i));
    return;
  }

  // The one-line state reply. First token `dev=jaw` so a reader can tell this
  // from the bar's `st amb=...` without knowing which address it came from.
  // Channel 1 is the header; `st ch=N` lines follow, one per channel.
  if (verb == "st") {
    Chan& c = C[0];
    float openD = c.p.closedDeg + c.p.dir * c.p.swingDeg;
    String out = "st dev=jaw channels=" + String(nch) + " closed=" + String(c.p.closedDeg, 1)
               + " swing=" + String(c.p.swingDeg, 1)
               + " dir=" + String((int)c.p.dir)
               + " open=" + String(openD, 1)
               + " lo=" + String(c.p.loDeg, 0) + " hi=" + String(c.p.hiDeg, 0)
               + " deg=" + String(c.angle, 1)
               + " us=" + String((int)degToUs(c.angle))
               + " env=" + String(c.env, 2)
               + " lvl=" + String(trackLevel, 2)
               + " hold=" + (c.holdDeg >= 0.0f ? String(c.holdDeg, 1) : String("-"))
               + (c.limited ? " limited=1" : "")
               + (c.attached ? "" : " detached=1")
               + " trk=" + String(trackFrames)
               + " clip=" + String(trackClip)
               + (playing ? " playing@" + String(trackPosMs()) : " idle");
    Serial.println(out);
    linkSend(out);
    for (int i = 0; i < nch; i++) {
      Chan& d = C[i];
      String o = "st ch=" + String(i + 1) + " mode=" + modeName(d) + " pos=" + posOf(d)
               + " auto=" + (d.autoOn ? "on" : "off") + " attached=" + (d.attached ? "1" : "0")
               + " deg=" + String(d.angle, 1);
      Serial.println(o);
      linkSend(o);
    }
    return;
  }

  if (verb == "save") {
    saveParams();
    String m = "saved params" + String(trackFrames ? " (track already on flash)" : "");
    reply(m);
    return;
  }

  if (verb == "forget") {
    clearParams();
    if (fsReady) LittleFS.remove(TRACK_PATH);
    trackFrames = 0; trackClip = 0; playing = false;
    for (int i = 1; i < MAX_CH; i++) detachServo(i);
    reply("forgot everything — compiled defaults, no track");
    return;
  }

  if (verb == "help") {
    Serial.println(F("show | get              every setting (a few lines over the radio)"));
    Serial.println(F("st                      state: header (channel 1) then one line per channel"));
    Serial.println(F("set [<ch>.]<key> <v>    change a setting (see show for keys)"));
    Serial.println(F("save | forget           keep settings across power cycles | back to defaults"));
    Serial.println(F("channels <1..4>         how many servos this board drives"));
    Serial.println(F("mode <ch> speech|manual   slot <ch> <n> <deg> <speed>|clear"));
    Serial.println(F("go <ch> <n>             go to position slot n   auto <ch> on|off"));
    Serial.println(F("speak <s> | env <0..1>  synthetic speech | hold a level;  idle releases"));
    Serial.println(F("hold [<ch>] <deg> | open [<ch>] | close [<ch>]   pin a servo (clamped)"));
    Serial.println(F("trk begin|d|end         load an envelope track"));
    Serial.println(F("cue <clip> <ms> <lead>  start it, on the box's clock"));
    Serial.println(F("pos <ms>                correction, eased; stop = release the track; stop all = also end every auto"));
    return;
  }

  if (verb == "channels") {
    int n = rest.toInt();
    if (n < 1 || n > MAX_CH) { reply("refused: channels must be 1.." + String(MAX_CH)); return; }
    for (int i = n; i < nch; i++) { C[i].autoOn = false; detachServo(i); }
    nch = n;
    reply("ok channels = " + String(nch));
    return;
  }

  if (verb == "set") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) { reply("set [<ch>.]<key> <value>"); return; }
    String k = rest.substring(0, s2);
    float v = rest.substring(s2 + 1).toFloat();
    int ch = 0;
    if (k.length() > 2 && k[0] >= '1' && k[0] <= '9' && k[1] == '.') {
      ch = chArg(k.substring(0, 1));
      if (ch < 0) { reply(noChannel(k.substring(0, 1))); return; }
      k = k.substring(2);
    }
    String note, m;
    if (setParam(C[ch], k, v, note)) {
      for (int i = 0; i < N_KNOBS; i++) if (k == KNOBS[i].name) m = "ok " + (ch ? String(ch + 1) + "." : String("")) + k + " = " + knobValue(C[ch], i);
    } else {
      m = note;
    }
    reply(m);
    return;
  }

  if (verb == "mode") {
    int s2 = rest.indexOf(' ');
    int ch = chArg(s2 < 0 ? rest : rest.substring(0, s2));
    String what = (s2 < 0) ? "" : rest.substring(s2 + 1);
    what.trim();
    if (ch < 0) { reply(noChannel(s2 < 0 ? rest : rest.substring(0, s2))); return; }
    if (what != "speech" && what != "manual") { reply("refused: mode is speech or manual"); return; }
    Chan& c = C[ch];
    c.x.mode = (what == "manual") ? 1.0f : 0.0f;
    c.autoOn = false; c.manualTarget = -1.0f; c.holdDeg = -1.0f; c.curSlot = 0;
    reply("ok mode " + String(ch + 1) + " " + what);
    return;
  }

  if (verb == "slot") {                         // slot <ch> <n> <deg> <speed> | slot <ch> <n> clear
    String a[4]; int na = 0; String t = rest;
    while (t.length() && na < 4) {
      int s2 = t.indexOf(' ');
      a[na++] = (s2 < 0) ? t : t.substring(0, s2);
      t = (s2 < 0) ? "" : t.substring(s2 + 1);
      t.trim();
    }
    int ch = (na >= 1) ? chArg(a[0]) : -1;
    if (ch < 0) { reply(noChannel(na ? a[0] : String("?"))); return; }
    int n = (na >= 2) ? a[1].toInt() : 0;
    if (n < 1 || n > MAX_SLOTS) { reply("refused: slot is 1.." + String(MAX_SLOTS)); return; }
    Chan& c = C[ch];
    if (na == 3 && a[2] == "clear") {
      c.x.slotDeg[n - 1] = -1.0f;
      reply("ok slot " + String(ch + 1) + " " + String(n) + " cleared");
      return;
    }
    if (na < 4) { reply("slot <ch> <n> <deg> <speed> | slot <ch> <n> clear"); return; }
    c.x.slotDeg[n - 1]   = constrain(a[2].toFloat(), 0.0f, 180.0f);
    c.x.slotSpeed[n - 1] = constrain(a[3].toFloat(), 30.0f, 1000.0f);
    reply("ok slot " + String(ch + 1) + " " + String(n) + " = " + String(c.x.slotDeg[n - 1], 0) + "/" + String(c.x.slotSpeed[n - 1], 0));
    return;
  }

  if (verb == "go") {
    int s2 = rest.indexOf(' ');
    String chs = (s2 < 0) ? rest : rest.substring(0, s2);
    int ch = chArg(chs);
    if (ch < 0) { reply(noChannel(chs)); return; }
    Chan& c = C[ch];
    if (c.x.mode < 0.5f) { reply("refused: channel " + String(ch + 1) + " is a speech channel"); return; }
    int n = (s2 < 0) ? 0 : rest.substring(s2 + 1).toInt();
    if (n < 1 || n > MAX_SLOTS) { reply("refused: slot is 1.." + String(MAX_SLOTS)); return; }
    if (slotDegOf(c, n) < 0.0f) { reply("refused: slot " + String(n) + " not defined on channel " + String(ch + 1)); return; }
    c.autoOn = false; c.holdDeg = -1.0f;       // an explicit position ends the repeat
    goSlot(c, n);
    reply("ok go " + String(ch + 1) + " " + String(n));
    return;
  }

  if (verb == "auto") {
    int s2 = rest.indexOf(' ');
    String chs = (s2 < 0) ? rest : rest.substring(0, s2);
    String what = (s2 < 0) ? "" : rest.substring(s2 + 1);
    what.trim();
    int ch = chArg(chs);
    if (ch < 0) { reply(noChannel(chs)); return; }
    Chan& c = C[ch];
    if (c.x.mode < 0.5f) { reply("refused: channel " + String(ch + 1) + " is a speech channel"); return; }
    if (what == "off") {
      if (c.autoOn) { c.autoOn = false; int a = (int)c.x.autoA; if (slotDegOf(c, a) >= 0.0f) goSlot(c, a); }
      reply("ok auto " + String(ch + 1) + " off");
      return;
    }
    if (what != "on") { reply("auto <ch> on|off"); return; }
    int a = (int)c.x.autoA, b = (int)c.x.autoB;
    if (slotDegOf(c, a) < 0.0f || slotDegOf(c, b) < 0.0f || a == b) {
      reply("refused: autoA/autoB (" + String(a) + "/" + String(b) + ") must be two different defined slots");
      return;
    }
    uint32_t now = millis();
    c.autoUntil = now + AUTO_DEADMAN_MS;       // every `auto on` re-arms the dead-man
    if (!c.autoOn) {                           // a repeat of `on` must not restart the cycle
      c.autoOn = true; c.holdDeg = -1.0f;
      goSlot(c, a);
      c.autoPhase = 0;
      float r = esp_random() / 4294967295.0f;
      c.autoNext = now + (uint32_t)fmaxf(200.0f, c.x.autoGapS * 1000.0f * (0.5f + r));
    }
    reply("ok auto " + String(ch + 1) + " on");
    return;
  }

  if (verb == "speak") {
    float secs = rest.length() ? rest.toFloat() : 4.0f;
    envHold = -1.0f; for (int i = 0; i < MAX_CH; i++) C[i].holdDeg = -1.0f;
    speakStart = millis();
    speakUntil = speakStart + (uint32_t)(secs * 1000);
    reply("speaking for " + String(secs, 1) + "s");
    return;
  }

  if (verb == "env") {
    envHold = constrain(rest.toFloat(), 0.0f, 1.0f);
    speakUntil = 0; for (int i = 0; i < MAX_CH; i++) C[i].holdDeg = -1.0f;
    reply("env held at " + String(envHold, 2));
    return;
  }

  if (verb == "idle") {
    envHold = -1.0f; speakUntil = 0; for (int i = 0; i < MAX_CH; i++) C[i].holdDeg = -1.0f;
    reply("idle — released");
    return;
  }

  // hold <deg> | hold <ch> <deg> ; open [<ch>] ; close [<ch>] — bench calibration
  if (verb == "hold" || verb == "open" || verb == "close") {
    int s2 = rest.indexOf(' ');
    int ch = 0;
    String degArg = rest;
    if (verb == "hold" && s2 >= 0) { ch = chArg(rest.substring(0, s2)); degArg = rest.substring(s2 + 1); }
    else if (verb != "hold" && rest.length()) ch = chArg(rest);
    if (ch < 0) { reply(noChannel(rest)); return; }
    Chan& c = C[ch];
    float d = (verb == "open")  ? c.p.closedDeg + c.p.dir * c.p.swingDeg
            : (verb == "close") ? c.p.closedDeg
            : degArg.toFloat();
    c.holdDeg = clampDeg(c, constrain(d, 0.0f, 180.0f));
    c.autoOn = false;
    envHold = -1.0f; speakUntil = 0;
    reply("hold " + (ch ? String(ch + 1) + ": " : String("")) + String(c.holdDeg, 1) + " deg");
    return;
  }

  // -- the track: begin / d / end, the same wire format as the bar ----------
  if (verb == "trk") {
    int sp2 = rest.indexOf(' ');
    String sub = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String args = (sp2 < 0) ? "" : rest.substring(sp2 + 1);

    if (sub == "begin") {
      int s3 = args.indexOf(' ');
      trackClip = (uint16_t)args.substring(0, s3).toInt();
      rxFrames = (uint16_t)args.substring(s3 + 1).toInt();
      rxBytes = 0; trackFrames = 0; playing = false;
      if (rxFrames == 0 || rxFrames > TRACK_MAX_FRAMES) {
        reply("trk err frames " + String(rxFrames) + " out of range");
        rxFrames = 0;
        return;
      }
      reply("trk begin ok clip=" + String(trackClip) + " frames=" + String(rxFrames));
      return;
    }

    if (sub == "d") {
      if (!rxFrames) { reply("trk err no begin"); return; }
      int s3 = args.indexOf(' ');
      uint32_t seq = (uint32_t)args.substring(0, s3).toInt();
      String payload = args.substring(s3 + 1);
      uint32_t at = seq * TRACK_CHUNK_BYTES;
      if (at != rxBytes) {                       // a lost chunk must not shift every later frame
        reply("trk err seq " + String(seq) + " expected byte " + String(rxBytes) + " got " + String(at));
        rxFrames = 0;
        return;
      }
      rxBytes += b64decode(payload.c_str(), trackBuf + rxBytes, sizeof(trackBuf) - rxBytes);
      linkQuiet();                               // 28 acks would flood and LOSE packets
      return;
    }

    if (sub == "end") {
      uint32_t want = (uint32_t)rxFrames * 2;
      if (rxBytes < want) {
        reply("trk err short " + String(rxBytes) + " of " + String(want));
        rxFrames = 0;
        return;
      }
      trackFrames = rxFrames;
      rxFrames = 0;
      bool saved = saveTrack();
      reply("trk ok " + String(trackClip) + " " + String(trackFrames) + (saved ? " saved" : " (not saved)"));
      return;
    }

    reply("trk err unknown " + sub);
    return;
  }

  // cue <clip> <position_ms> <lead_ms>, sent at ACTUAL playback start.
  if (verb == "cue") {
    if (!trackFrames) { reply("cue err no track"); return; }
    int a1 = rest.indexOf(' ');
    int a2 = rest.indexOf(' ', a1 + 1);
    uint16_t clip = (uint16_t)rest.substring(0, a1).toInt();
    int32_t posMs = rest.substring(a1 + 1, a2 < 0 ? rest.length() : a2).toInt();
    int32_t lead = (a2 < 0) ? 0 : rest.substring(a2 + 1).toInt();
    if (clip != trackClip) {
      reply("cue err clip " + String(clip) + " but loaded " + String(trackClip));
      return;
    }
    cueLocalMs = millis();
    cueTrackMs = posMs + lead;
    playing = true;
    envHold = -1.0f; speakUntil = 0; for (int i = 0; i < MAX_CH; i++) C[i].holdDeg = -1.0f;
    reply("cue ok " + String(clip) + " at " + String(posMs) + " lead " + String(lead));
    return;
  }

  // pos <ms>: EASE toward it, never jump. A visible correction is worse than the
  // drift it fixes. Arrives every ~2 s and must not ack.
  if (verb == "pos") {
    if (!playing) return;
    int32_t err = rest.toInt() - trackPosMs();
    cueTrackMs += err / 4;
    linkQuiet();
    return;
  }

  // `stop` releases the TRACK only: the box sends it at the end of every spoken
  // line, in the middle of a show, and a blink must survive that. `stop all` is
  // for the end of a show and also ends every channel's `auto`.
  if (verb == "stop" || verb == "eof") {
    playing = false;
    if (rest == "all") {
      for (int i = 0; i < MAX_CH; i++) {
        Chan& c = C[i];
        if (c.autoOn) { c.autoOn = false; int a = (int)c.x.autoA; if (slotDegOf(c, a) >= 0.0f) goSlot(c, a); }
      }
      Serial.println(F("track: released, every auto stopped"));
    } else {
      Serial.println(F("track: released"));
    }
    return;
  }

  Serial.println("? " + verb + "  (try: help)");
}

static void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { handleLine(buf); buf = ""; }
    else if (buf.length() < 80)  buf += c;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);                  // never block on a port nobody is reading
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }

  for (int i = 1; i < MAX_CH; i++) { C[i].x.mode = 1.0f; }   // a second servo is rarely a mouth
  for (int i = 0; i < MAX_CH; i++) DEF[i] = C[i];
  loadParams();

  // Straight to the closed angle, before the filesystem and the radio, so the
  // servo does not wait on either to reach rest. (Whatever it is doing when power
  // arrives, it is whatever the signal pin did before this line.)
  for (int i = 0; i < nch; i++) {
    pinMode(CH_PINS[i], OUTPUT);
    digitalWrite(CH_PINS[i], LOW);
    C[i].angle = clampDeg(C[i], C[i].p.closedDeg);
    writeAngle(i, C[i].angle);
  }

  fsReady = LittleFS.begin(true);
  bool restored = loadTrack();
  linkBegin();

  Serial.println();
  Serial.println(F("=== jaw — servo remote ==================================="));
  for (int i = 0; i < nch; i++)
    Serial.printf("ch %d (%s): closed %.1f deg, swing %.1f deg (dir %+d), limits %.0f..%.0f\n",
                  i + 1, modeName(C[i]).c_str(), C[i].p.closedDeg, C[i].p.swingDeg, (int)C[i].p.dir,
                  C[i].p.loDeg, C[i].p.hiDeg);
  Serial.print  (F("storage  : "));
  if (!fsReady)        Serial.println(F("LittleFS UNAVAILABLE — a power cycle will lose the track"));
  else if (restored)   Serial.printf("track restored, clip %u, %u frames\n", trackClip, trackFrames);
  else                 Serial.println(F("ready, no stored track"));
  Serial.print  (F("my MAC   : ")); Serial.println(WiFi.macAddress());
  Serial.print  (F("link     : ")); Serial.println(linkUp ? F("ESP-NOW up, ch 1") : F("DOWN — serial only"));
  Serial.println(F("commands : show | st | set <k> <v> | speak <s> | hold <deg> | go <ch> <n> | help"));
  Serial.println(F("On a speech channel, idle is speech with a zero envelope."));
  Serial.println(F("=========================================================="));
}

void loop() {
  static uint32_t lastUs = micros();
  pollSerial();
  pumpTx();

  if (rxPending) {
    String line(rxBuf);
    rxPending = false;
    if (line.startsWith("trk d ")) { Serial.print(F("rf> ")); Serial.println(line.substring(0, 12)); }
    else                           { Serial.print(F("rf> ")); Serial.println(line); }
    linkReplied = false;
    handleLine(line);
    if (!linkReplied) linkSend("ack " + line);
  }

  uint32_t nowUs = micros();
  if (nowUs - lastUs < FRAME_US) return;
  float dt = (nowUs - lastUs) / 1000000.0f;
  lastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;

  feedSpeech(dt);
  for (int i = 0; i < nch; i++) render(i, dt);
}
