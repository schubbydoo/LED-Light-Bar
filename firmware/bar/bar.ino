/*
  The light bar — a named ambience, excited by speech.

  Built from docs/01-effect-design.md. Read that before changing anything here;
  it explains not just what the four layers are but which failure each one is
  preventing, and the failures are the point. Every way an LED strip announces
  itself as an LED strip is a shortcut that looks reasonable in code.

  ---------------------------------------------------------------------------
  THE ONE STRUCTURAL DECISION

      heat[i] = clamp(EMBER_FLOOR,
                      (noise[i] + flare[i]) * breath * (1 + speechGain * env),
                      1.0)

  When `env` is zero that expression collapses exactly to the idle fire. So
  there is **no talking mode**, no crossfade, and no state to get wrong — idle
  is speech with a zero envelope. This is why the design doc insists on writing
  it this way rather than as two renderers with a blend between them. A prop
  that switches to a "speaking" animation stops being a fire that speaks and
  becomes a lamp on a dimmer, and you cannot fix that with better parameters.

  The corollary is that the box never sends colours. It sends a scalar. What
  orange means is this renderer's business and nobody else's.

  ---------------------------------------------------------------------------
  THE FOUR LAYERS

    0  ember floor    the fire never goes out. Black reads as a fault, not as a
                      fire, and a borrowed prop that looks broken is worse than
                      one that looks wrong.
    1  breath         one low-frequency value multiplying the WHOLE strip. This
                      is the dominant cue and the thing most fire effects miss —
                      real firelight brightens and dims as a body as the flame
                      draws air. Tune this first.
    2  spatial noise  correlated 1D noise, so neighbouring pixels move TOGETHER.
                      Independent per-pixel randomness is glitter, and the eye
                      reads glitter as "LEDs" instantly.
    3  flares         Poisson-timed crackles: fast attack, slow release. The
                      pops that stop it feeling looped.

  Colour is a pure function of heat — never randomised, and the top of the ramp
  is warm amber rather than white, because pure white reads as a camera flash.
  Gamma is applied last; without it the low end vanishes and smooth fades look
  stepped, which is the usual answer to "why does it look steppy".

  ---------------------------------------------------------------------------
  TUNING IT

  Every parameter below is live over serial, because all of them are judged by
  eye and reflashing to try a number breaks the comparison you are holding in
  your head:

      ./send.sh bar show
      ./send.sh bar "set breath 1.4"
      ./send.sh bar "speak 4"          synthetic speech, no radio needed
      ./send.sh bar "env 0.8"          hold excitation to judge the top end

  Nothing is persisted. The board boots on the defaults compiled in here, on
  purpose — a bar that lives in a borrowed prop must not retain state that
  surprises its owner later. When a number is right it comes back here.

  Judge it in real darkness, at the real distance, on the real surface. A fire
  effect assessed on a bench under room lights will mislead you completely.
*/

#include <FastLED.h>

#define DATA_PIN      D10
#define NUM_LEDS      50
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB
#define MAX_MILLIAMPS 1500

// 100 FPS. The design doc asks for 60-100; the noise field wants the headroom
// and the C3 drives the strip from the RMT peripheral, so this costs nothing.
static const uint8_t  FPS = 100;
static const uint32_t FRAME_US = 1000000UL / FPS;

CRGB leds[NUM_LEDS];

// ---------------------------------------------------------------------------
// Ambience — the light equivalent of the ambient audio voice.
//
// The box selects one BY NAME and never describes it. `fire` and `water` differ
// entirely in here; swapping them is one string in a show script and nothing
// upstream knows what changed. That indirection earns its keep because the prop
// changes every season and the hardware does not.
//
// Only `fire` has a designed character. The other two carry honest palettes but
// their motion is still the fire's, so they read as coloured fire rather than as
// water or storm. They are declared so the interface is real, not so they are
// finished — see docs/01 for what each still needs.
// ---------------------------------------------------------------------------

DEFINE_GRADIENT_PALETTE(pal_fire) {
      0,  40,   0,   0,     // dying ember
     64, 120,  10,   0,     // deep red
    128, 200,  50,   0,     // orange
    191, 255, 110,  10,     // bright flame
    255, 255, 190,  80      // white-hot tip — warm, NOT white
};
DEFINE_GRADIENT_PALETTE(pal_water) {
      0,   0,   8,  30,
     64,   0,  40,  90,
    128,   0,  90, 150,
    191,  30, 160, 200,
    255, 140, 230, 240
};
DEFINE_GRADIENT_PALETTE(pal_storm) {
      0,   4,   4,  10,
     64,  16,  18,  40,
    128,  50,  55,  90,
    191, 130, 140, 180,
    255, 235, 240, 255
};

struct Ambience { const char* name; const TProgmemRGBGradientPalette_byte* pal; };
static const Ambience AMBIENCES[] = {
    {"fire",  pal_fire},
    {"water", pal_water},
    {"storm", pal_storm},
};
static const uint8_t N_AMBIENCE = sizeof(AMBIENCES) / sizeof(AMBIENCES[0]);
static uint8_t ambIndex = 0;
static CRGBPalette16 palette = pal_fire;

// ---------------------------------------------------------------------------
// The knobs. Defaults are docs/01 §8; every one is settable at runtime.
// ---------------------------------------------------------------------------
struct Params {
  float emberFloor  = 0.12f;   // the fire never goes out
  float breathHz    = 1.1f;    // whole-strip breathing rate — TUNE FIRST
  float breathDepth = 0.25f;   // how far the breath swings brightness
  float spaceScale  = 18.0f;   // noise units per pixel; ~3 features across 50
  float timeScale   = 130.0f;  // noise units per second — slow is right
  float flareMeanS  = 3.5f;    // mean seconds between crackles
  float flareRelease= 0.6f;    // seconds for a crackle to fade
  float flareWidth  = 4.0f;    // pixels, gaussian sigma
  float speechGain  = 1.2f;    // how hard speech drives the fire
  float speechSpread= 0.4f;    // how much loud speech widens the bright zone
  float attack      = 0.5f;    // syllable crispness
  float release     = 0.08f;   // thermal inertia — MOST CHARACTER-DEFINING
  float gamma       = 2.2f;    // perceptual smoothness
  uint8_t brightness= 160;
} P;

static uint8_t gammaLUT[256];
static void buildGamma() {
  for (int i = 0; i < 256; i++)
    gammaLUT[i] = (uint8_t)(powf(i / 255.0f, P.gamma) * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// Flares — a small fixed pool. Poisson arrivals, fast attack, slow release.
// ---------------------------------------------------------------------------
static const uint8_t MAX_FLARES = 5;
static const float   FLARE_ATTACK_S = 0.04f;
struct Flare { bool live; float pos, amp, age; };
static Flare flares[MAX_FLARES];

static void spawnFlare(float amp, float atPos = -1.0f) {
  for (uint8_t f = 0; f < MAX_FLARES; f++) {
    if (flares[f].live) continue;
    flares[f] = {true,
                 atPos >= 0 ? atPos : (random16() / 65535.0f) * (NUM_LEDS - 1),
                 amp, 0.0f};
    return;
  }
}

// ---------------------------------------------------------------------------
// Speech. Two scalars, and the box sends nothing else.
//
//   env   [0,1]  how hard the speech is driving, right now
//   onset [0,1]  transient strength, for hits on plosives
//
// Today they come from `speak` (a synthetic envelope) or `env` (a manual hold).
// Tomorrow they come off the radio. Nothing below this line will change when
// they do — which is the point of keeping the renderer ignorant of transport.
// ---------------------------------------------------------------------------
static float env = 0.0f;          // smoothed
static float envTarget = 0.0f;    // raw, before asymmetric smoothing
static float envHold = -1.0f;     // >=0 means a manual hold is in force
static uint32_t speakUntil = 0;   // millis deadline for the synthetic test
static uint32_t speakStart = 0;

static void feedSpeech(float dt) {
  float raw = 0.0f;

  if (envHold >= 0.0f) {
    raw = envHold;
  } else if (speakUntil && millis() < speakUntil) {
    // A stand-in for real speech: syllables at ~4 Hz inside phrases, with a
    // gap every couple of seconds so the fire visibly settles between them.
    // Not a recording — just enough structure to judge attack, release and
    // gain before any envelope exists to play.
    float t = (millis() - speakStart) / 1000.0f;
    bool inPhrase = fmodf(t, 2.6f) < 1.9f;
    if (inPhrase) {
      float syl = 0.5f + 0.5f * sinf(t * 2.0f * PI * 4.0f);
      raw = powf(syl, 1.6f) * (0.55f + 0.45f * sinf(t * 1.7f));
      if (raw < 0.0f) raw = 0.0f;
    }
  } else if (speakUntil && millis() >= speakUntil) {
    speakUntil = 0;
    Serial.println(F("speak: done"));
  }

  envTarget = raw;

  // Asymmetric smoothing. Fast attack keeps syllable onsets crisp; slow release
  // gives the flame thermal inertia, because real fire does not stop instantly.
  // Raw RMS is far too twitchy to look like fire without this.
  float prev = env;
  float k = (raw > env) ? P.attack : P.release;
  // Scale the coefficients to the frame rate so changing FPS does not silently
  // change the character of the effect.
  k = 1.0f - powf(1.0f - k, dt * FPS);
  env += (raw - env) * k;

  // Onset: half-wave rectified rise. A sharp climb spawns a crackle, which is
  // what makes consonants visible.
  float rise = env - prev;
  if (rise > 0.02f) spawnFlare(fminf(1.0f, rise * 14.0f));
}

// ---------------------------------------------------------------------------

static float heat[NUM_LEDS];

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setBrightness(P.brightness);
  buildGamma();
  random16_set_seed((uint16_t)esp_random());

  Serial.println();
  Serial.println(F("=== light bar — ambience renderer ========================"));
  Serial.print  (F("ambience : ")); Serial.println(AMBIENCES[ambIndex].name);
  Serial.print  (F("pixels   : ")); Serial.println(NUM_LEDS);
  Serial.println(F("commands : show | set <k> <v> | amb <name> | speak <s> |"));
  Serial.println(F("           env <0..1> | idle | help"));
  Serial.println(F("Idle is speech with a zero envelope — there is no mode."));
  Serial.println(F("=========================================================="));
}

// --- serial ---------------------------------------------------------------

static void showParams() {
  Serial.print(F("ambience      ")); Serial.println(AMBIENCES[ambIndex].name);
  Serial.print(F("emberFloor    ")); Serial.println(P.emberFloor, 3);
  Serial.print(F("breathHz      ")); Serial.println(P.breathHz, 3);
  Serial.print(F("breathDepth   ")); Serial.println(P.breathDepth, 3);
  Serial.print(F("spaceScale    ")); Serial.println(P.spaceScale, 2);
  Serial.print(F("timeScale     ")); Serial.println(P.timeScale, 2);
  Serial.print(F("flareMeanS    ")); Serial.println(P.flareMeanS, 2);
  Serial.print(F("flareRelease  ")); Serial.println(P.flareRelease, 3);
  Serial.print(F("flareWidth    ")); Serial.println(P.flareWidth, 2);
  Serial.print(F("speechGain    ")); Serial.println(P.speechGain, 3);
  Serial.print(F("speechSpread  ")); Serial.println(P.speechSpread, 3);
  Serial.print(F("attack        ")); Serial.println(P.attack, 3);
  Serial.print(F("release       ")); Serial.println(P.release, 3);
  Serial.print(F("gamma         ")); Serial.println(P.gamma, 2);
  Serial.print(F("brightness    ")); Serial.println(P.brightness);
  Serial.print(F("env (live)    ")); Serial.println(env, 3);
}

static bool setParam(const String& k, float v) {
  if      (k == "emberFloor")   P.emberFloor   = v;
  else if (k == "breathHz" || k == "breath") P.breathHz = v;
  else if (k == "breathDepth")  P.breathDepth  = v;
  else if (k == "spaceScale")   P.spaceScale   = v;
  else if (k == "timeScale")    P.timeScale    = v;
  else if (k == "flareMeanS")   P.flareMeanS   = v;
  else if (k == "flareRelease") P.flareRelease = v;
  else if (k == "flareWidth")   P.flareWidth   = v;
  else if (k == "speechGain")   P.speechGain   = v;
  else if (k == "speechSpread") P.speechSpread = v;
  else if (k == "attack")       P.attack       = v;
  else if (k == "release")      P.release      = v;
  else if (k == "gamma")      { P.gamma = v; buildGamma(); }
  else if (k == "brightness") { P.brightness = (uint8_t)v;
                                FastLED.setBrightness(P.brightness); }
  else return false;
  return true;
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""   : line.substring(sp + 1);
  rest.trim();

  if (verb == "show" || verb == "get") { showParams(); return; }

  if (verb == "help") {
    Serial.println(F("show                 every live parameter"));
    Serial.println(F("set <key> <value>    change one (see show for keys)"));
    Serial.println(F("amb <fire|water|storm>"));
    Serial.println(F("speak <seconds>      synthetic speech envelope"));
    Serial.println(F("env <0..1>           hold excitation; 'idle' releases"));
    Serial.println(F("idle                 release the hold, stop speaking"));
    Serial.println(F("flare                fire one crackle now"));
    return;
  }

  if (verb == "set") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) { Serial.println(F("set <key> <value>")); return; }
    String k = rest.substring(0, s2);
    float v = rest.substring(s2 + 1).toFloat();
    Serial.println(setParam(k, v) ? "ok " + k + " = " + String(v, 3)
                                  : "no such parameter: " + k);
    return;
  }

  if (verb == "amb") {
    for (uint8_t i = 0; i < N_AMBIENCE; i++) {
      if (rest == AMBIENCES[i].name) {
        ambIndex = i;
        palette = AMBIENCES[i].pal;
        Serial.println("ambience = " + rest);
        return;
      }
    }
    Serial.println(F("ambience must be fire, water or storm"));
    return;
  }

  if (verb == "speak") {
    float secs = rest.length() ? rest.toFloat() : 4.0f;
    envHold = -1.0f;
    speakStart = millis();
    speakUntil = speakStart + (uint32_t)(secs * 1000);
    Serial.println("speaking for " + String(secs, 1) + "s");
    return;
  }

  if (verb == "env") {
    envHold = constrain(rest.toFloat(), 0.0f, 1.0f);
    speakUntil = 0;
    Serial.println("env held at " + String(envHold, 2));
    return;
  }

  if (verb == "idle") {
    envHold = -1.0f; speakUntil = 0;
    Serial.println(F("idle — envelope released"));
    return;
  }

  if (verb == "flare") { spawnFlare(1.0f); Serial.println(F("flare")); return; }

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

// --- the frame ------------------------------------------------------------

void loop() {
  static uint32_t lastUs = micros();
  pollSerial();

  uint32_t nowUs = micros();
  if (nowUs - lastUs < FRAME_US) return;
  float dt = (nowUs - lastUs) / 1000000.0f;
  lastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;          // a long stall must not jump the sim

  feedSpeech(dt);

  float tSec = millis() / 1000.0f;

  // Layer 1 — breath. One value for the whole strip. Low-frequency noise rather
  // than a sine, because a periodic breath is audible to the eye as a loop.
  uint16_t bz = (uint16_t)(tSec * P.breathHz * 256.0f);
  float breathRaw = inoise8(0, bz) / 255.0f;
  float breath = (1.0f - P.breathDepth) + P.breathDepth * breathRaw;

  // Speech agitates the fire as well as brightening it: zones widen and drift
  // faster while it is talking. Both collapse to the idle values at env = 0.
  float spaceScale = P.spaceScale * (1.0f - P.speechSpread * 0.5f * env);
  float timeScale  = P.timeScale  * (1.0f + 0.6f * env);

  // Layers 3 — advance the crackles.
  float flareField[NUM_LEDS] = {0};
  for (uint8_t f = 0; f < MAX_FLARES; f++) {
    if (!flares[f].live) continue;
    flares[f].age += dt;
    float e;
    if (flares[f].age < FLARE_ATTACK_S) {
      e = flares[f].age / FLARE_ATTACK_S;
    } else {
      e = expf(-(flares[f].age - FLARE_ATTACK_S) / P.flareRelease);
      if (e < 0.02f) { flares[f].live = false; continue; }
    }
    float a = flares[f].amp * e;
    float inv2s2 = 1.0f / (2.0f * P.flareWidth * P.flareWidth);
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      float d = i - flares[f].pos;
      flareField[i] += a * expf(-(d * d) * inv2s2);
    }
  }

  // Poisson arrivals for the idle crackle. Independent of onset flares, so the
  // fire keeps its own life even in silence.
  if (P.flareMeanS > 0.01f &&
      (random16() / 65535.0f) < (dt / P.flareMeanS))
    spawnFlare(0.35f + 0.45f * (random16() / 65535.0f));

  // Layer 2 — correlated spatial noise, then the whole expression.
  uint16_t tz = (uint16_t)(tSec * timeScale);
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    float n = inoise8((uint16_t)(i * spaceScale), tz) / 255.0f;
    float h = (n + flareField[i]) * breath * (1.0f + P.speechGain * env);
    heat[i] = h < P.emberFloor ? P.emberFloor : (h > 1.0f ? 1.0f : h);
  }

  // Colour is a function of heat and nothing else, then gamma last.
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    CRGB c = ColorFromPalette(palette, (uint8_t)(heat[i] * 255.0f), 255, LINEARBLEND);
    leds[i] = CRGB(gammaLUT[c.r], gammaLUT[c.g], gammaLUT[c.b]);
  }
  FastLED.show();
}
