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

  Pin: D10 (GPIO10). Never D0, D8 or D9 — see CLAUDE.md, strapping pins.
*/

#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const int      SERVO_PIN = D10;
static const uint32_t PWM_HZ    = 50;
static const uint8_t  PWM_BITS  = 14;                      // 16384 counts per 20 ms
static const uint32_t PERIOD_US = 1000000UL / PWM_HZ;

// Datasheet mapping (docs/07-servo-s51.md): 500 us = 0 deg, 2500 us = 180 deg.
// A pulse outside this range is never produced, whatever is typed.
static const float US_MIN = 500.0f, US_MAX = 2500.0f;

static const uint8_t  FPS      = 50;                       // the servo's frame rate AND the track's
static const uint32_t FRAME_US = 1000000UL / FPS;

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
static bool linkReplied = false;
static void linkQuiet() { linkReplied = true; }

static void linkSend(const String& s) {
  if (!linkUp) return;
  esp_now_send(PEER_BRIDGE, (const uint8_t*)s.c_str(), s.length());
  linkReplied = true;
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
// Parameters. All floats, one blob, with a version AND a size — see bar.ino for
// why. A table drives `show`, `set` and `get`, so adding a knob is one line.
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
static Params P;
static Params DEFAULTS;

struct Knob { const char* name; float* p; float lo, hi; };
static Knob KNOBS[] = {
  {"closedDeg",   &P.closedDeg,   0.0f,  180.0f},
  {"swingDeg",    &P.swingDeg,    0.0f,   90.0f},
  {"dir",         &P.dir,        -1.0f,    1.0f},
  {"loDeg",       &P.loDeg,       0.0f,  180.0f},
  {"hiDeg",       &P.hiDeg,       0.0f,  180.0f},
  {"gain",        &P.gain,        0.0f,    2.0f},
  {"deadband",    &P.deadband,    0.0f,    0.9f},
  {"curve",       &P.curve,       0.3f,    3.0f},
  {"attack",      &P.attack,      0.01f,   1.0f},
  {"release",     &P.release,     0.01f,   1.0f},
  {"slewDegS",    &P.slewDegS,   30.0f,  1000.0f},
  {"lookaheadMs", &P.lookaheadMs, 0.0f,  300.0f},
  {"idleDetachS", &P.idleDetachS, 0.0f,  600.0f},
};
static const int N_KNOBS = sizeof(KNOBS) / sizeof(KNOBS[0]);

static Preferences prefs;
static const char*    NVS_NS         = "jaw";
static const uint16_t PARAMS_VERSION = 1;

static void saveParams() {
  prefs.begin(NVS_NS, false);
  prefs.putUShort("pver", PARAMS_VERSION);
  prefs.putUShort("psize", (uint16_t)sizeof(Params));
  prefs.putBytes("params", &P, sizeof(Params));
  prefs.end();
}

static void loadParams() {
  prefs.begin(NVS_NS, true);
  uint16_t ver  = prefs.getUShort("pver", 0);
  uint16_t size = prefs.getUShort("psize", 0);
  if (ver == PARAMS_VERSION && size == sizeof(Params)) {
    Params tmp;
    if (prefs.getBytes("params", &tmp, sizeof(tmp)) == sizeof(tmp)) P = tmp;
  } else if (ver) {
    Serial.printf("params: stored v%u/%uB ignored (this build wants v%u/%uB)\n",
                  ver, size, PARAMS_VERSION, (unsigned)sizeof(Params));
  }
  prefs.end();
}

static void clearParams() {
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  P = DEFAULTS;
}

// 0 = ok, else a reason. Kept apart from the clamp so "no such parameter" keeps
// meaning exactly one thing, the way the bar's does.
static bool setParam(const String& k, float v, String& note) {
  for (int i = 0; i < N_KNOBS; i++) {
    if (k != KNOBS[i].name) continue;
    v = constrain(v, KNOBS[i].lo, KNOBS[i].hi);
    if (k == "dir") v = (v < 0.0f) ? -1.0f : 1.0f;
    float oldLo = P.loDeg, oldHi = P.hiDeg;
    *KNOBS[i].p = v;
    if (P.loDeg + 5.0f > P.hiDeg) {            // the window must stay a window
      P.loDeg = oldLo; P.hiDeg = oldHi;
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
// transport; it steps whatever track it is handed.
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
static float env = 0.0f;            // smoothed level, 0..1
static float trackLevel = 0.0f;     // raw level this frame
static float envHold = -1.0f;       // >=0: a manual level hold (`env`)
static float holdDeg = -1.0f;       // >=0: pinned at an angle (`hold`, `open`, `close`)
static uint32_t speakUntil = 0, speakStart = 0;
static bool  speechActive = false;

static float angleDeg = 90.0f;      // what is being commanded now, after slew
static bool  attached = false;
static bool  limited = false;       // the last target was clipped by loDeg/hiDeg
static uint32_t restSince = 0;

static float clampDeg(float d) {
  float lo = fmaxf(0.0f, P.loDeg), hi = fminf(180.0f, P.hiDeg);
  if (d < lo) d = lo;
  if (d > hi) d = hi;
  return d;
}

static float degToUs(float d) {
  float us = US_MIN + d * (US_MAX - US_MIN) / 180.0f;
  return constrain(us, US_MIN, US_MAX);
}

static void writeAngle(float d) {
  if (!attached) { ledcAttach(SERVO_PIN, PWM_HZ, PWM_BITS); attached = true; }
  uint32_t duty = (uint32_t)(((double)degToUs(d) * ((1u << PWM_BITS) - 1)) / PERIOD_US);
  ledcWrite(SERVO_PIN, duty);
}

static void detachServo() {
  if (!attached) return;
  ledcDetach(SERVO_PIN);
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  attached = false;
}

static float shape(float e) {
  float x = (e - P.deadband) / fmaxf(0.01f, 1.0f - P.deadband);
  if (x < 0.0f) x = 0.0f;
  x = powf(fminf(x, 1.0f), P.curve) * P.gain;
  return fminf(x, 1.0f);
}

static void feedSpeech(float dt) {
  float raw = 0.0f;
  trackLevel = 0.0f;
  if (envHold >= 0.0f) {
    raw = envHold;
    trackLevel = envHold;
  } else if (playing) {
    int32_t pos = trackPosMs() + (int32_t)P.lookaheadMs;
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

  float k = (raw > env) ? P.attack : P.release;
  k = 1.0f - powf(1.0f - k, dt * FPS);
  env += (raw - env) * k;
  if (env < 0.002f) env = 0.0f;
}

static void render(float dt) {
  float target;
  if (holdDeg >= 0.0f) target = holdDeg;
  else target = P.closedDeg + P.dir * P.swingDeg * shape(env);
  float clamped = clampDeg(target);
  limited = fabsf(clamped - target) > 0.01f;
  target = clamped;

  // The most the servo is ASKED to move. A real S51 cannot follow a raw 50 Hz
  // envelope, and asking it to is how a servo chatters and wears.
  float maxStep = P.slewDegS * dt;
  float d = target - angleDeg;
  if (d >  maxStep) d =  maxStep;
  if (d < -maxStep) d = -maxStep;
  angleDeg += d;

  bool atRest = fabsf(angleDeg - clampDeg(P.closedDeg)) < 0.05f && !speechActive && holdDeg < 0.0f;
  if (!atRest) restSince = millis();
  if (P.idleDetachS > 0.0f && atRest && millis() - restSince > (uint32_t)(P.idleDetachS * 1000.0f)) {
    detachServo();
    return;
  }
  writeAngle(angleDeg);
}

// ---------------------------------------------------------------------------
// Commands.
// ---------------------------------------------------------------------------

static String knobValue(int i) {
  char b[16];
  snprintf(b, sizeof(b), "%g", *KNOBS[i].p);
  return String(b);
}

static void showParams() {
  for (int i = 0; i < N_KNOBS; i++) {
    Serial.print(KNOBS[i].name);
    for (int pad = strlen(KNOBS[i].name); pad < 13; pad++) Serial.print(' ');
    Serial.println(knobValue(i));
  }
  Serial.print(F("env (live)   ")); Serial.println(env, 3);
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""   : line.substring(sp + 1);
  rest.trim();

  // `show` prints one knob per line on serial and answers over the radio with a
  // single line of key=value pairs, so the web UI can read every setting without
  // opening the board's serial port (which reboots it).
  if (verb == "show" || verb == "get") {
    showParams();
    String out = "show";
    for (int i = 0; i < N_KNOBS; i++) out += String(" ") + KNOBS[i].name + "=" + knobValue(i);
    linkSend(out);
    return;
  }

  // The one-line state reply. First token `dev=jaw` so a reader can tell this
  // from the bar's `st amb=...` without knowing which address it came from.
  if (verb == "st") {
    float openD = P.closedDeg + P.dir * P.swingDeg;
    String out = "st dev=jaw closed=" + String(P.closedDeg, 1)
               + " swing=" + String(P.swingDeg, 1)
               + " dir=" + String((int)P.dir)
               + " open=" + String(openD, 1)
               + " lo=" + String(P.loDeg, 0) + " hi=" + String(P.hiDeg, 0)
               + " deg=" + String(angleDeg, 1)
               + " us=" + String((int)degToUs(angleDeg))
               + " env=" + String(env, 2)
               + " lvl=" + String(trackLevel, 2)
               + " hold=" + (holdDeg >= 0.0f ? String(holdDeg, 1) : String("-"))
               + (limited ? " limited=1" : "")
               + (attached ? "" : " detached=1")
               + " trk=" + String(trackFrames)
               + " clip=" + String(trackClip)
               + (playing ? " playing@" + String(trackPosMs()) : " idle");
    Serial.println(out);
    linkSend(out);
    return;
  }

  if (verb == "save") {
    saveParams();
    String m = "saved params" + String(trackFrames ? " (track already on flash)" : "");
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "forget") {
    clearParams();
    if (fsReady) LittleFS.remove(TRACK_PATH);
    trackFrames = 0; trackClip = 0; playing = false;
    String m = "forgot everything — compiled defaults, no track";
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "help") {
    Serial.println(F("show | get              every setting (one line over the radio)"));
    Serial.println(F("st                      state: angle, level, track, clip"));
    Serial.println(F("set <key> <value>       change a setting (see show for keys)"));
    Serial.println(F("save | forget           keep settings across power cycles | back to defaults"));
    Serial.println(F("speak <s> | env <0..1>  synthetic speech | hold a level;  idle releases"));
    Serial.println(F("hold <deg> | open | close   pin the jaw (clamped to loDeg..hiDeg)"));
    Serial.println(F("trk begin|d|end         load an envelope track"));
    Serial.println(F("cue <clip> <ms> <lead>  start it, on the box's clock"));
    Serial.println(F("pos <ms>                correction, eased; stop  release to idle"));
    return;
  }

  if (verb == "set") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) { Serial.println(F("set <key> <value>")); linkSend("set <key> <value>"); return; }
    String k = rest.substring(0, s2);
    float v = rest.substring(s2 + 1).toFloat();
    String note;
    String m;
    if (setParam(k, v, note)) {
      for (int i = 0; i < N_KNOBS; i++) if (k == KNOBS[i].name) m = "ok " + k + " = " + knobValue(i);
    } else {
      m = note;
    }
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "speak") {
    float secs = rest.length() ? rest.toFloat() : 4.0f;
    envHold = -1.0f; holdDeg = -1.0f;
    speakStart = millis();
    speakUntil = speakStart + (uint32_t)(secs * 1000);
    String m = "speaking for " + String(secs, 1) + "s";
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "env") {
    envHold = constrain(rest.toFloat(), 0.0f, 1.0f);
    speakUntil = 0; holdDeg = -1.0f;
    String m = "env held at " + String(envHold, 2);
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "idle") {
    envHold = -1.0f; speakUntil = 0; holdDeg = -1.0f;
    String m = "idle — released";
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "hold" || verb == "open" || verb == "close") {
    float d = (verb == "open")  ? P.closedDeg + P.dir * P.swingDeg
            : (verb == "close") ? P.closedDeg
            : rest.toFloat();
    holdDeg = clampDeg(constrain(d, 0.0f, 180.0f));
    envHold = -1.0f; speakUntil = 0;
    String m = "hold " + String(holdDeg, 1) + " deg";
    Serial.println(m); linkSend(m);
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
        String e = "trk err frames " + String(rxFrames) + " out of range";
        Serial.println(e); linkSend(e);
        rxFrames = 0;
        return;
      }
      String m = "trk begin ok clip=" + String(trackClip) + " frames=" + String(rxFrames);
      Serial.println(m); linkSend(m);
      return;
    }

    if (sub == "d") {
      if (!rxFrames) { String e = "trk err no begin"; Serial.println(e); linkSend(e); return; }
      int s3 = args.indexOf(' ');
      uint32_t seq = (uint32_t)args.substring(0, s3).toInt();
      String payload = args.substring(s3 + 1);
      uint32_t at = seq * TRACK_CHUNK_BYTES;
      if (at != rxBytes) {                       // a lost chunk must not shift every later frame
        String e = "trk err seq " + String(seq) + " expected byte " + String(rxBytes) + " got " + String(at);
        Serial.println(e); linkSend(e);
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
        String e = "trk err short " + String(rxBytes) + " of " + String(want);
        Serial.println(e); linkSend(e);
        rxFrames = 0;
        return;
      }
      trackFrames = rxFrames;
      rxFrames = 0;
      bool saved = saveTrack();
      String m = "trk ok " + String(trackClip) + " " + String(trackFrames) + (saved ? " saved" : " (not saved)");
      Serial.println(m); linkSend(m);
      return;
    }

    String e = "trk err unknown " + sub;
    Serial.println(e); linkSend(e);
    return;
  }

  // cue <clip> <position_ms> <lead_ms>, sent at ACTUAL playback start.
  if (verb == "cue") {
    if (!trackFrames) { String e = "cue err no track"; Serial.println(e); linkSend(e); return; }
    int a1 = rest.indexOf(' ');
    int a2 = rest.indexOf(' ', a1 + 1);
    uint16_t clip = (uint16_t)rest.substring(0, a1).toInt();
    int32_t posMs = rest.substring(a1 + 1, a2 < 0 ? rest.length() : a2).toInt();
    int32_t lead = (a2 < 0) ? 0 : rest.substring(a2 + 1).toInt();
    if (clip != trackClip) {
      String e = "cue err clip " + String(clip) + " but loaded " + String(trackClip);
      Serial.println(e); linkSend(e); return;
    }
    cueLocalMs = millis();
    cueTrackMs = posMs + lead;
    playing = true;
    envHold = -1.0f; speakUntil = 0; holdDeg = -1.0f;
    String m = "cue ok " + String(clip) + " at " + String(posMs) + " lead " + String(lead);
    Serial.println(m); linkSend(m);
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

  if (verb == "stop" || verb == "eof") {
    playing = false;
    Serial.println(F("track: released"));
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

  DEFAULTS = P;
  loadParams();

  // Straight to the closed angle, before the filesystem and the radio, so the
  // jaw does not wait on either to reach rest. (Whatever the servo is doing when
  // power arrives, it is whatever the signal pin did before this line.)
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  angleDeg = clampDeg(P.closedDeg);
  writeAngle(angleDeg);

  fsReady = LittleFS.begin(true);
  bool restored = loadTrack();
  linkBegin();

  Serial.println();
  Serial.println(F("=== jaw — servo mouth ===================================="));
  Serial.printf ("closed %.1f deg, swing %.1f deg (dir %+d), limits %.0f..%.0f\n",
                 P.closedDeg, P.swingDeg, (int)P.dir, P.loDeg, P.hiDeg);
  Serial.print  (F("storage  : "));
  if (!fsReady)        Serial.println(F("LittleFS UNAVAILABLE — a power cycle will lose the track"));
  else if (restored)   Serial.printf("track restored, clip %u, %u frames\n", trackClip, trackFrames);
  else                 Serial.println(F("ready, no stored track"));
  Serial.print  (F("my MAC   : ")); Serial.println(WiFi.macAddress());
  Serial.print  (F("link     : ")); Serial.println(linkUp ? F("ESP-NOW up, ch 1") : F("DOWN — serial only"));
  Serial.println(F("commands : show | st | set <k> <v> | speak <s> | hold <deg> | open | close | help"));
  Serial.println(F("Idle is speech with a zero envelope — there is no mode."));
  Serial.println(F("=========================================================="));
}

void loop() {
  static uint32_t lastUs = micros();
  pollSerial();

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
  render(dt);
}
