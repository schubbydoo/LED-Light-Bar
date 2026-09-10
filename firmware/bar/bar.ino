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
  is warm yellow rather than white, because pure white reads as a camera flash.

  **Per-channel gamma is OFF by default, against docs/01's advice, and the
  reason is written out at buildGamma().** Short version: gamma decode is correct
  for colours authored in sRGB, this palette is authored as PWM values, and
  applying it a second time crushes the small green component out of every warm
  colour — turning the whole ramp one step redder. It is still tunable, because
  the fade-smoothness problem the doc describes is real; the fix for that is to
  gamma the intensity, not each channel.

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
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

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
// The link.
//
// ESP-NOW delivers TEXT LINES, and they go into the same `handleLine()` the
// serial port uses. So every command this sketch understands works over the
// radio too, with no second protocol to keep in step and no second parser to
// drift. `amb water` typed at the bridge does exactly what `amb water` typed
// here does.
//
// Both ends must be on the same channel. Neither is associated with an access
// point, so nothing negotiates it — it is fixed at both ends and must match.
// A mismatch is not an error: the send succeeds and the delivery callback
// reports failure, which reads as "the other board is off".
// ---------------------------------------------------------------------------
static uint8_t PEER_BRIDGE[6] = {0xF0, 0x9E, 0x9E, 0xB2, 0x3D, 0x38};
static const uint8_t LINK_CHANNEL = 1;
static bool linkUp = false;

static void handleLine(String line);          // fwd

// Set whenever a handler has already answered by air, so the generic ack below
// does not fire a second packet on its heels.
//
// ESP-NOW sends are asynchronous. Queuing a second before the first has
// completed loses BOTH — which showed up as `st` replying perfectly over serial
// and saying nothing at all over the radio, while the delivery callback still
// reported success. One reply per received line.
static bool linkReplied = false;

// Mark a line answered without answering. Some commands must NOT reply: a
// track load is 28 chunks and a position update arrives every 2s, and acking
// those floods the link — which is not merely noisy, it LOSES packets, because
// ESP-NOW sends are asynchronous and one queued on top of another takes both
// down. The first track push failed exactly this way: all 28 chunks arrived and
// the `trk end` confirmation was lost among the acks they provoked.
static void linkQuiet() { linkReplied = true; }

static void linkSend(const String& s) {
  if (!linkUp) return;
  esp_now_send(PEER_BRIDGE, (const uint8_t*)s.c_str(), s.length());
  linkReplied = true;
}

// The radio callback runs in WiFi task context, so it must not touch FastLED or
// block. Copy the line out and let loop() act on it.
static volatile bool  rxPending = false;
static char           rxBuf[250];
static volatile int   rxLen = 0;

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (rxPending) return;                       // drop rather than tear a line
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

// Orange is the body of a fire. Red belongs to the embers underneath it and
// yellow to the sparks coming off it — so the ramp reaches orange EARLY and
// spends most of its length there, rather than treating orange as a waypoint on
// the road to white.
//
// That matters because of where the simulation actually sits. Idle heat averages
// about 0.44 — noise around 0.5, breath around 0.875 — so index ~112 is the
// colour you see most of the time, and it has to be orange. The first version
// put a red-orange there and reached orange only at the peaks, which reads as a
// red fire with occasional orange rather than an orange fire with red embers.
// The useful way to read this ramp is the GREEN-TO-RED RATIO, because that is
// the whole red-orange-yellow axis and it is the only thing your eye is judging:
//
//     g/r ~ 0.1   red            g/r ~ 0.45  orange
//     g/r ~ 0.30  red-orange     g/r ~ 0.70  amber
//                                g/r ~ 0.90  yellow
//
// The body of the fire sits near index 112 (idle heat averages ~0.44), so that
// entry has to land around 0.43. The peaks are where flares put it, and the top
// entry wants to be AMBER, not yellow: the previous top was g/r 0.90 and every
// crackle read as a yellow flash.
DEFINE_GRADIENT_PALETTE(pal_fire) {
      0,  50,   0,   0,     // ember bed — deep red, never black
     40, 130,  18,   0,     // red                       g/r 0.14
     95, 215,  85,   0,     // orange arrives            g/r 0.40
    145, 250, 120,   4,     // ORANGE — the body         g/r 0.48
    205, 255, 150,  15,     // bright orange             g/r 0.59
    255, 255, 175,  40      // amber spark — NOT yellow  g/r 0.69
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
  // 1.0 = off, and off is correct for this palette. See buildGamma().
  float gamma       = 1.0f;
  // Scales the green channel of whatever the palette returned, so it slides the
  // WHOLE ramp along the red-orange-yellow axis without redefining it. 1.0 is
  // the palette as authored; 0.75 reads distinctly redder, 1.3 distinctly more
  // yellow. This exists because that axis is the only thing anyone ever wants to
  // adjust by eye, and asking for a recompile per attempt makes it unfindable.
  float yellow      = 1.0f;
  // Deliberately below half. On a pale surface the failure is almost never "not
  // bright enough" — it is that the wall blows out and the colour washes to
  // cream. Less light reads as MORE saturated. Push it up on a dark, matte,
  // warm-toned surface where it has somewhere to go.
  uint8_t brightness= 110;
} P;

// Gamma, and why it defaults to OFF.
//
// docs/01 says to apply gamma as the last step before writing pixels, and for
// smooth *fades* that advice is right. Applied per channel to THIS palette it is
// wrong, and wrong in a way that looks like a design error rather than a bug:
//
//     (200, 50, 0) "orange"  --gamma 2.2-->  (149, 7, 0)   near-pure red
//     (255,110,10) "flame"   --gamma 2.2-->  (255, 40, 0)  deep red-orange
//     (255,190,80) "tip"     --gamma 2.2-->  (255,133,20)  merely orange
//
// Green is small in every warm colour, and gamma crushes small values hardest,
// so every entry loses its green and the whole ramp shifts one step redder. The
// fire came out red with orange peaks instead of orange with red embers.
//
// The cause is a space mismatch: gamma DECODE is the right transform for colours
// authored in sRGB, and this palette — like FastLED's own HeatColors_p — is
// authored as direct PWM values. Correcting it a second time double-corrects.
//
// Left tunable rather than deleted, because the fade-smoothness claim is real
// and worth testing on the actual surface: `set gamma 2.2` restores the old
// behaviour for a side-by-side. If banding ever shows up in the low end, the fix
// is to gamma the INTENSITY while leaving hue alone, not to gamma each channel.
static uint8_t gammaLUT[256];
static void buildGamma() {
  for (int i = 0; i < 256; i++)
    gammaLUT[i] = (P.gamma == 1.0f)
                    ? (uint8_t)i
                    : (uint8_t)(powf(i / 255.0f, P.gamma) * 255.0f + 0.5f);
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
// The envelope track, and the clock that steps it.
//
// The track is RESIDENT before playback starts. It is never streamed. 2 bytes
// per frame at 50 Hz is 100 B/s, so a 41 s greeting is 4.1 kB — one burst at
// provisioning time. During the performance the link carries a cue, a position
// update every couple of seconds, and nothing else, which means **packet loss
// during a greeting is not a failure mode that exists**. Streaming would put a
// radio on the show's critical path for the whole line to save 4 kB.
//
// THE CLOCK IS THE BOX'S, NOT OURS. `cue` carries the position read from the
// playback handle at *actual* playback start, and everything here derives from
// that. This is the one rule §5.7 exists to enforce: Ghost Host starts its
// player and its motor thread separately, each taking its own time reference,
// and carries an uncorrected offset forever. We never start a clock of our own.
// ---------------------------------------------------------------------------

static const uint16_t TRACK_MAX_FRAMES = 8000;         // 160 s at 50 Hz
static const uint8_t  FRAME_MS = 20;                   // 50 Hz, matches the box

// Bytes of envelope per `trk d` chunk. 150 raw becomes 200 base64 characters,
// and the line around it fits inside ESP-NOW's 250-byte payload with room to
// spare. The SENDER must use the same number — it is how a sequence number is
// turned back into a byte offset. firmware/README.md carries it too.
static const uint32_t TRACK_CHUNK_BYTES = 150;

static uint8_t  trackBuf[TRACK_MAX_FRAMES * 2];
static uint16_t trackFrames = 0;                       // frames actually loaded
static uint16_t trackClip = 0;
static uint16_t rxFrames = 0;                          // expected, during load
static uint32_t rxBytes = 0;                           // written so far

static bool     playing = false;
static uint32_t cueLocalMs = 0;    // millis() when the cue arrived
static int32_t  cueTrackMs = 0;    // track position at that instant, incl lead

// Small, table-free base64. The payload is text so the whole wire stays
// readable — a chunk that arrives mangled is visible as mangled rather than
// silently decoding to plausible garbage, which matters far more here than the
// third of a packet the encoding costs.
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;                       // '=' padding and anything unexpected
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
  // Trailing '=' means the last group carried fewer than three bytes. The
  // caller knows the true length from `trk begin`, so trimming here would be
  // guesswork; it truncates against the declared frame count instead.
  return n;
}

// Where in the track we are, right now, derived entirely from the box's cue.
static int32_t trackPosMs() {
  return (int32_t)(millis() - cueLocalMs) + cueTrackMs;
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

// The onset channel has a measurement FLOOR, and it is not zero. RMS over
// 320-sample frames does not land on the period of the signal inside it, so the
// level wobbles through even a held tone and the difference channel sees it —
// measured at 7/255 = 0.027 on the box. A threshold of 0.02 therefore fires on
// silence-shaped noise and the fire crackles continuously through steady
// speech, which reads as the effect being noisy rather than as the threshold
// being wrong. Sit clear of the floor.
static const float ONSET_GATE = 0.06f;

static void feedSpeech(float dt) {
  float raw = 0.0f;

  // Priority: a manual hold, then a real track, then the synthetic stand-in.
  // The track wins over `speak` so a cue arriving mid-demo does the right thing.
  if (envHold >= 0.0f) {
    raw = envHold;
  } else if (playing) {
    int32_t pos = trackPosMs();
    if (pos < 0) {
      raw = 0.0f;                       // cued early — lead_ms can be negative
    } else {
      uint32_t f = (uint32_t)pos / FRAME_MS;
      if (f >= trackFrames) {
        // The track ran out. Release rather than stop dead: `release` carries
        // the fire back down over its own time constant, which is what a fire
        // does. Completion is still an EVENT from the box (`stop`) for the
        // aborted case — this is only the natural end.
        playing = false;
        Serial.println(F("track: finished"));
      } else {
        raw = trackBuf[f * 2] / 255.0f;
        float on = trackBuf[f * 2 + 1] / 255.0f;
        if (on > ONSET_GATE) spawnFlare(fminf(1.0f, on * 1.4f));
      }
    }
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

  // Onset derived locally from the rise, for the synthetic and held cases. A
  // real track carries its own onset channel and already spawned above — doing
  // both would double every crackle.
  if (!playing) {
    float rise = env - prev;
    if (rise > 0.02f) spawnFlare(fminf(1.0f, rise * 14.0f));
  }
}

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
  linkBegin();

  Serial.println();
  Serial.println(F("=== light bar — ambience renderer ========================"));
  Serial.print  (F("ambience : ")); Serial.println(AMBIENCES[ambIndex].name);
  Serial.print  (F("pixels   : ")); Serial.println(NUM_LEDS);
  Serial.print  (F("my MAC   : ")); Serial.println(WiFi.macAddress());
  Serial.print  (F("link     : "));
  Serial.println(linkUp ? F("ESP-NOW up, ch 1") : F("DOWN — serial only"));
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
  Serial.print(F("yellow        ")); Serial.println(P.yellow, 3);
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
  else if (k == "yellow")       P.yellow       = v;
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

  // A one-line state summary, and the only reply that goes back over the radio.
  //
  // It exists because reading the bar's state over its own serial port is
  // self-defeating: opening that port reboots the C3, so what you read back is
  // always the compiled defaults, not what you just set. Over the radio the bar
  // is never interrupted, so `st` is the only honest way to ask it anything.
  if (verb == "st") {
    String out = "st amb=" + String(AMBIENCES[ambIndex].name)
               + " y=" + String(P.yellow, 2)
               + " br=" + String(P.brightness)
               + " breath=" + String(P.breathHz, 2)
               + " rel=" + String(P.release, 3)
               + " gain=" + String(P.speechGain, 2)
               + " env=" + String(env, 2)
               + " trk=" + String(trackFrames)
               + (playing ? " playing@" + String(trackPosMs()) : " idle");
    Serial.println(out);
    linkSend(out);
    return;
  }

  if (verb == "help") {
    Serial.println(F("show                 every live parameter (serial only)"));
    Serial.println(F("st                   one-line state, answers over the radio"));
    Serial.println(F("set <key> <value>    change one (see show for keys)"));
    Serial.println(F("amb <fire|water|storm>"));
    Serial.println(F("speak <seconds>      synthetic speech envelope"));
    Serial.println(F("env <0..1>           hold excitation; 'idle' releases"));
    Serial.println(F("set yellow 0.8       redder | 1.3 more yellow"));
    Serial.println(F("set brightness 70    less light reads as MORE saturated"));
    Serial.println(F("idle                 release the hold, stop speaking"));
    Serial.println(F("flare                fire one crackle now"));
    Serial.println(F("trk begin|d|end      load an envelope track"));
    Serial.println(F("cue <clip> <ms> <lead>   start it, on the box's clock"));
    Serial.println(F("pos <ms>             correction; eased, never jumped"));
    Serial.println(F("stop                 release to idle"));
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

  // -- the track ----------------------------------------------------------
  //
  //    trk begin <clip> <frames>     start a load, clears whatever was here
  //    trk d <seq> <base64>          one chunk of level/onset pairs
  //    trk end                       -> "trk ok <clip> <frames>" or "trk err ..."
  //
  // `seq` is checked rather than trusted. A dropped chunk would otherwise shift
  // every later frame earlier and produce a track that plays perfectly and is
  // silently out of time — the worst outcome available, because it looks like
  // bad sync rather than like a lost packet.
  if (verb == "trk") {
    int sp2 = rest.indexOf(' ');
    String sub = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String args = (sp2 < 0) ? "" : rest.substring(sp2 + 1);

    if (sub == "begin") {
      int s3 = args.indexOf(' ');
      trackClip = (uint16_t)args.substring(0, s3).toInt();
      rxFrames = (uint16_t)args.substring(s3 + 1).toInt();
      rxBytes = 0;
      trackFrames = 0;
      playing = false;
      if (rxFrames == 0 || rxFrames > TRACK_MAX_FRAMES) {
        String e = "trk err frames " + String(rxFrames) + " out of range";
        Serial.println(e); linkSend(e);
        rxFrames = 0;
        return;
      }
      String m = "trk begin ok clip=" + String(trackClip) +
                 " frames=" + String(rxFrames);
      Serial.println(m); linkSend(m);
      return;
    }

    if (sub == "d") {
      if (!rxFrames) { String e = "trk err no begin"; Serial.println(e);
                       linkSend(e); return; }
      int s3 = args.indexOf(' ');
      uint32_t seq = (uint32_t)args.substring(0, s3).toInt();
      String payload = args.substring(s3 + 1);
      uint32_t at = seq * TRACK_CHUNK_BYTES;
      if (at != rxBytes) {
        String e = "trk err seq " + String(seq) + " expected byte " +
                   String(rxBytes) + " got " + String(at);
        Serial.println(e); linkSend(e);
        rxFrames = 0;
        return;
      }
      uint32_t n = b64decode(payload.c_str(), trackBuf + rxBytes,
                             sizeof(trackBuf) - rxBytes);
      rxBytes += n;
      linkQuiet();                     // 28 acks would flood the link, not just
      return;                          // clutter it — see linkQuiet()
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
      String m = "trk ok " + String(trackClip) + " " + String(trackFrames);
      Serial.println(m); linkSend(m);
      return;
    }

    String e = "trk err unknown " + sub;
    Serial.println(e); linkSend(e);
    return;
  }

  // -- the cue --------------------------------------------------------------
  //
  //    cue <clip> <position_ms> <lead_ms>
  //
  // Sent at ACTUAL playback start, carrying the position read from the playback
  // handle. `lead_ms` is a measured per-channel trim, negative to run the light
  // ahead of the sound: the Pi's audio-start jitter depends on its ALSA path,
  // and light arriving slightly early reads as natural where late reads as
  // broken. Tunable without a reflash, which is the whole point of it being a
  // field in the message rather than a constant in here.
  if (verb == "cue") {
    if (!trackFrames) { String e = "cue err no track"; Serial.println(e);
                        linkSend(e); return; }
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
    String m = "cue ok " + String(clip) + " at " + String(posMs) +
               " lead " + String(lead);
    Serial.println(m); linkSend(m);
    return;
  }

  // -- the correction -------------------------------------------------------
  //
  //    pos <position_ms>
  //
  // EASE toward it, never jump. A visible correction is worse than the drift it
  // fixes — the C3's crystal is good for well under a millisecond across a 40 s
  // line, so a large error here means something real went wrong (the audio was
  // paused, the cue was late) and snapping to it would show as the fire
  // flinching. A quarter of the error per update converges in a few seconds and
  // is invisible.
  if (verb == "pos") {
    if (!playing) return;
    int32_t err = rest.toInt() - trackPosMs();
    cueTrackMs += err / 4;
    linkQuiet();                        // arrives every ~2s; must not ack
    return;
  }

  // Completion is an EVENT, not a duration. An aborted greeting settles the fire
  // correctly because it was told, rather than because a timer expired.
  if (verb == "stop" || verb == "eof") {
    playing = false;
    Serial.println(F("track: released"));
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

  // A line off the radio. Same parser, same effect — the transport is not the
  // renderer's business. The ack is what lets the box know a cue landed, which
  // a one-way 433 channel could never do.
  if (rxPending) {
    String line(rxBuf);
    rxPending = false;
    Serial.print(F("rf> ")); Serial.println(line);
    linkReplied = false;
    handleLine(line);
    // Only ack when the command did not already answer for itself. `st` sends
    // its own state; everything else gets this.
    if (!linkReplied) linkSend("ack " + line);
  }

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
    if (P.yellow != 1.0f) {
      float g = c.g * P.yellow;
      c.g = (uint8_t)(g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g));
    }
    leds[i] = CRGB(gammaLUT[c.r], gammaLUT[c.g], gammaLUT[c.b]);
  }
  FastLED.show();
}
