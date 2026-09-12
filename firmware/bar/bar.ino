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
#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define DATA_PIN      D10

// HOW LONG THE BAR IS — a ceiling here, the actual length in `P.numLeds`.
//
// MAX_LEDS sizes the BUFFERS and is a compile-time fact: it is the longest run
// this build can drive, and it is 144 because that is the whole 1 m strip on
// hand at 144 LED/m. The length actually rendered is `P.numLeds`, which is live
// and persisted — `set leds 96`, or `len 24` in inches — because the diffuser
// is flexible and the bar gets contoured to whatever prop it visits. A season
// that wants 20" of it should not need a toolchain.
//
// Pixels from `P.numLeds` to MAX_LEDS are written BLACK every frame rather than
// left out of the frame. FastLED clocks the whole buffer either way, so leaving
// them alone would hold the last image on the tail forever; and the clocked-out
// bytes simply fall off the end of a shorter physical strip, which costs a few
// hundred microseconds and nothing else.
//
// Almost nothing in the renderer scales with the count — `spaceScale` and
// `flareWidth` are per-pixel, so the fire keeps its grain at any length and
// simply gets more or less of it (see the notes on both). `flareMeanS` is the
// exception and is per-STRIP: a shorter bar wants a LARGER value to read the
// same, and it is not scaled automatically because it is judged by eye. What
// does scale, and scales hard, is the electrical side:
//
//   * DRAW IS PER PIXEL, so it tracks the length directly. MEASURED on the
//     prop 2026-09-12, 144 px, fire at brightness 110: a USB meter on the
//     supply reads 5.05 V / ~0.80 A — about 4 W for the whole build, XIAO
//     included, and ~40 % of the TalentCell's 2 A ceiling. Shorten the bar and
//     that falls proportionally. `pwr` prints the estimate for the CURRENT
//     length so a meter has something to disagree with, and `st` carries it
//     too — because reading it over serial reboots the board and answers about
//     the defaults instead.
//   * THE CAP STILL DOES NOT BITE — and this corrects a prediction made here
//     the same morning. The arithmetic said 144 px at brightness 110 would sit
//     within a few percent of the 1500 mA cap, so the bright frames would get
//     held down. MEASURED, it does not come close: `pwr` sampled fourteen times
//     across the resting fire reported 887-1185 mA, and `allowed` equalled
//     `brightness` on every one. The prediction came from 0.12 W/LED, which is
//     the figure for WHITE; this fire is amber and red at partial heat and
//     never goes white.
//
//     SPEECH LOWERS THE DRAW, which is the other half of why the peaks never
//     arrived. `blackout` is the primary speech signal, so between words all
//     144 pixels go dark while the word flash brightens only a few: sampled
//     during `speak`, the range was 427-987 mA — the LOWEST of the session.
//
//     The warning is still worth keeping, because it becomes true as
//     `brightness` rises: if the fire ever looks like it is fighting itself,
//     check `pwr` for LIMITING and lower `brightness` rather than raising
//     `maxMA` past what the supply can deliver.
//
// VERIFYING THE LENGTH: nothing in the firmware can tell whether `numLeds`
// matches the strip. `ends` lights the first and last pixel of the current
// length and nothing between — set a length, run `ends`, and move it until the
// far marker sits where the bar ends. `bringup` stage 4 and `pixelcheck`'s
// markers do the same thing at MAX_LEDS, for a strip with no radio on it yet.
#define MAX_LEDS      144
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB
// The compiled default for `maxMA`, which is live — see Params. 1500 and not
// 1700, because the cap governs only the LEDs and the XIAO needs the rest.
#define MAX_MILLIAMPS 1500

// 100 FPS. The design doc asks for 60-100; the noise field wants the headroom
// and the C3 drives the strip from the RMT peripheral, so this costs nothing.
//
// The arithmetic is worth writing down because it is a wall rather than a
// slope, and it is set by MAX_LEDS rather than by `numLeds`: the whole buffer
// is clocked out whatever the bar's length, so shortening the bar does NOT buy
// frame time. WS2812B clocks at 800 kHz, so one frame is MAX_LEDS * 24 bits =
// 4.3 ms at 144 — inside the 10 ms budget but no longer a rounding error (it
// was 1.5 ms at 50). Raising MAX_LEDS to a second metre would put it at 8.6 ms
// and 100 FPS would stop being achievable — at which point lower FPS is the
// answer, not a faster loop.
static const uint8_t  FPS = 100;
static const uint32_t FRAME_US = 1000000UL / FPS;

CRGB leds[MAX_LEDS];

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

// What feeds heat. AMBIENT is the four-layer fire and every other value is a
// lamp behaviour. Speech still excites whichever is selected, so a blinking red
// lamp that also speaks is a legitimate thing to ask for.
enum {
  MOTION_AMBIENT = 0,   // the ambience's own layers — noise, breath, flares
  MOTION_SOLID   = 1,   // steady, full
  MOTION_BLINK   = 2,   // square wave at blinkHz
  MOTION_BREATHE = 3,   // the breath layer alone, no noise and no flares
};
static const char* MOTION_NAMES[] = {"ambient", "solid", "blink", "breathe"};
static const uint8_t N_MOTION = 4;

// Built at runtime from lampHue/lampSat, so the box can name any colour. Black
// at the bottom of the ramp is what makes the existing heat layers work as
// brightness: at heat 0 a lamp is off, at heat 1 it is the chosen colour.
static CRGBPalette16 lampPal;
static bool usingLamp = false;

// Defined after Params and `palette`, which it needs. Declared here so the
// command handlers above can call it.
static void buildLampPalette();
static CRGBPalette16 palette = pal_fire;

// ---------------------------------------------------------------------------
// The knobs. Defaults are docs/01 §8; every one is settable at runtime.
// ---------------------------------------------------------------------------
struct Params {
  // ---- the BAR ITSELF ---------------------------------------------------
  // Not a look — the physical length of the run, in pixels, at 144 LED/m. It
  // lives in Params because it has to survive a power cycle (`save`) and be
  // settable over the radio: the diffuser is flexible, so the bar is contoured
  // to each prop and its length is a property of the SEASON, not of the build.
  // Clamped to MAX_LEDS on the way in; the tail beyond it is blacked each frame.
  uint16_t numLeds  = MAX_LEDS;
  float emberFloor  = 0.12f;   // the fire never goes out
  float breathHz    = 1.1f;    // whole-strip breathing rate — TUNE FIRST
  float breathDepth = 0.45f;   // how far the breath swings brightness
  // Per PIXEL, which makes it a PHYSICAL scale rather than a fraction of the
  // strip: at 144 LED/m, 18 puts a noise feature every ~4-5 inches whether the
  // run is 14" or 39". So going to the whole strip keeps the fire's grain and
  // gives it ~8 features instead of ~3, rather than stretching three of them
  // over three times the length — which is what a fire three times as long
  // actually does. Raising this shrinks features; it does not add them.
  float spaceScale  = 18.0f;   // noise units per pixel; a feature every ~4-5"
  float timeScale   = 130.0f;  // noise units per second — slow is right
  // The rate is per STRIP, not per inch — and this is the one tuned value the
  // longer run genuinely invalidates. 3.5 s was judged by eye across 14"; the
  // same rate across 39" is a third of the crackle density, so the fire reads
  // calmer than the one that was approved. ~1.2 restores the old density, but
  // it is left at the approved number because it is judged by eye on the real
  // surface and silently retuning it would hide the change.
  float flareMeanS  = 3.5f;    // mean seconds between crackles, whole strip
  float flareRelease= 0.6f;    // seconds for a crackle to fade
  float flareWidth  = 4.0f;    // pixels, gaussian sigma
  // Lower than it was. Red now carries the speech, so the heat surge only has
  // to keep the fire alive underneath rather than be the signal itself — the
  // brief was "the underlying fire breathing" with red on top, not a fire that
  // doubles in brightness on every word.
  float speechGain  = 0.6f;    // how hard speech drives the fire
  // 0, not 0.4. This scales a SPATIAL noise coordinate, so modulating it
  // rescales the pattern along the strip — a zoom — and a zoom driven by a
  // speech envelope reads as the strip scrubbing rather than as zones widening.
  // The time axis had the same fault and was fixable by integrating the rate;
  // this axis is not, because there is no rate to integrate. See the note in
  // the render loop. Kept as a parameter because a slow envelope can use it.
  float speechSpread= 0.0f;    // how much loud speech widens the bright zone
  float attack      = 0.5f;    // syllable crispness
  float release     = 0.08f;   // thermal inertia — MOST CHARACTER-DEFINING
  // 1.0 = off, and off is correct for this palette. See buildGamma().
  float gamma       = 1.0f;
  float onsetGate   = 0.10f;   // below this, an onset is measurement noise
  // THE WORD FLASH. A heat surge moves along the palette, so a word arrives as
  // "somewhat more orange" — true to how a fire behaves and, watching it, not
  // distinct enough to read as speech. This pulses the WHOLE STRIP toward
  // saturated red on each word start: a hue change and a brightness change at
  // once, against an orange bed, which is very hard to miss.
  //
  // docs/01 §4 anticipated exactly this choice — build the heat-surge version,
  // try the red-flash version, keep whichever wins on the real surface. 0 turns
  // it off and leaves the pure heat mapping.
  // 0.6, not 0.9 — approved on the prop 2026-09-10. At 0.9 the speech colour
  // very nearly replaces the fire and reads as a separate light switching on;
  // at 0.6 the flame shows through it, so the bright parts of the fire come
  // through warmer than the dim parts and the strip keeps its own life while a
  // word is lit. "The fire goes red", not "a red light turns on".
  float flash       = 0.60f;   // how much of the speech colour a word mixes in
  // The speech colour, as a FastLED hue. Blue by default — not what a fire does,
  // and worth trying anyway: against a warm orange bed a cold hue is the largest
  // possible contrast, so a word reads at a glance rather than as a brightness
  // change on the same ramp. docs/01's rule is to keep whichever wins on the
  // real surface, and that argument does not care which colour won.
  //
  //     0 red    32 orange   64 yellow   96 green
  //   128 aqua  160 blue    192 purple  224 pink
  float flashHue    = 0.0f;    // red
  // BLACKOUT BETWEEN WORDS — but only while a track is playing.
  //
  // This deliberately breaks the ember-floor rule for the duration of a
  // greeting: docs/01 keeps the fire lit because "dark reads as a fault, not a
  // fire", and that is right for an unattended prop. During speech the reading
  // is different — the strip is not failing, it is punctuating, and a guest who
  // just heard a voice has every reason to read darkness as deliberate.
  //
  // The IDLE fire is untouched. Between greetings the bar still breathes, so the
  // prop never sits dark on its own. 1.0 = fully dark between words, 0 = the
  // fire keeps burning underneath as before.
  // 0.6, not 1.0 — approved on the prop 2026-09-10. Full blackout made the
  // gaps emphatic but left the greeting's 14s of drums completely dark before
  // the first word, which reads as a broken prop rather than as one waiting.
  // 0.6 keeps a low fire alive through the intro and still drops hard enough
  // between words to punctuate them.
  float blackout    = 0.6f;
  // How long a gap must last before the blackout RELEASES and the ambience
  // comes back. This is what lets one number mean two things.
  //
  // Steve's two requirements looked contradictory for an afternoon: darkness
  // between words is what makes speech read (brightness contrast reads, hue
  // contrast against a similar hue does not — measured by eye, repeatedly), but
  // a greeting that opens with 13s of drums must not sit dark through it,
  // because that reads as a broken prop rather than one waiting.
  //
  // They are only contradictory if `blackout` applies equally to a 0.4s pause
  // and a 13s silence. A PAUSE should be punctuated; a SILENCE should be
  // filled. So the blackout holds for `blackoutHold` and then eases off over
  // `blackoutEase`. Phrase gaps run 0.30-0.76s on the real greetings and stay
  // fully dark; the drum intro is fully lit within a couple of seconds.
  float blackoutHold= 0.9f;    // seconds of silence before the fire returns
  float blackoutEase= 1.2f;    // seconds to bring it back over
  float flashDecay  = 0.10f;   // seconds for red to leave after a word ends
  // Spawn a crackle at each word start as well. OFF: with red already marking
  // every word, the flares on top read as "too much flashing" — two events for
  // one word. Kept because a crackle on an emphasised word may earn its place
  // once the red is tuned.
  float wordFlare   = 0.0f;
  float flareGap    = 0.28f;   // seconds; the rate cap that stops the blinking
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
  // THE LED CURRENT CAP, live, because at 144 pixels it became a number you set
  // with a meter in your hand and a recompile per attempt makes that
  // unmeasurable. FastLED scales the WHOLE FRAME to fit it, so this is a
  // ceiling on the effect as much as on the battery.
  //
  // Raising it past what the supply can actually deliver buys no light: it buys
  // a brownout, and a brownout part-way along a WS2812B strip reads as random
  // colour rather than as dimming. The TalentCell's USB output is 2 A and the
  // XIAO lives on the same rail, so ~1700 is the hard ceiling and 1500 is the
  // one with margin in it.
  uint16_t maxMA    = MAX_MILLIAMPS;

  // ---- the LAMP ---------------------------------------------------------
  // A prop does not always want speech. Sometimes it wants a red light that
  // comes on, or a blue one that breathes. That is not a second renderer: a
  // lamp is an ambience whose palette ramps from BLACK to a chosen colour, so
  // "solid" and "breathing" fall straight out of the existing layers and only
  // blinking is new. The four-layer expression is untouched — `motion` only
  // decides what feeds heat.
  //
  // This is also the one place the box sends a COLOUR rather than a name, and
  // that is deliberate rather than a breach of the division: for `fire`,
  // `water` and `storm` the renderer owns what the name looks like, and the box
  // must not know. For a lamp the colour IS the request.
  uint8_t lampHue   = 0;       // FastLED hue; 0 red, 96 green, 160 blue
  uint8_t lampSat   = 255;     // 0 is white
  uint8_t motion    = 0;       // MOTION_AMBIENT; see the enum
  float   blinkHz   = 1.0f;    // blinks per second, for MOTION_BLINK
  float   blinkDuty = 0.5f;    // fraction of each cycle lit
} P;

static void buildLampPalette() {
  lampPal = CRGBPalette16(CRGB::Black, CHSV(P.lampHue, P.lampSat, 255));
  if (usingLamp) palette = lampPal;
}

// ---------------------------------------------------------------------------
// PERSISTENCE. The reason this exists, in one sentence: the track lived in RAM,
// so a battery blip mid-show left the bar silently dark until the box's next
// cooldown push — and during a day of firmware work it disarmed the prop three
// times in a row, each time looking exactly like the effect being broken.
//
// Two stores, because the two kinds of state want opposite things:
//
//   PARAMS -> NVS, and only on an explicit `save`. Auto-saving every `set`
//             would write flash on every nudge of a tuning slider, and tuning
//             is dozens of nudges. An explicit save also matches how the thing
//             is actually judged: try a value, look at the strip, keep it.
//
//   TRACK  -> LittleFS, written automatically the moment a load succeeds. The
//             box pushes one rarely and never during a show, so there is no
//             wear question, and nobody should have to remember to save the
//             thing that makes the prop work.
// ---------------------------------------------------------------------------

static void buildGamma();          // defined below; clearParams needs it

static Preferences prefs;
static const char*    NVS_NS        = "bar";
// Bumped to 2 when the lamp fields joined Params, to 3 when `maxMA` did, and to
// 4 when `numLeds` did. A stored older blob is ignored rather than
// reinterpreted — see loadParams(). The size guard would have caught these on
// its own; the version is bumped anyway, because a saved cap or length that no
// longer means what it did is exactly the kind of value that is individually
// plausible and collectively wrong.
static const uint16_t PARAMS_VERSION = 4;

// The compiled values, captured before anything is loaded over them. `forget`
// needs somewhere to go back to, and re-deriving them would mean maintaining a
// second copy of every default.
static Params DEFAULTS;

static void saveParams() {
  prefs.begin(NVS_NS, false);
  prefs.putUShort("pver", PARAMS_VERSION);
  prefs.putUShort("psize", (uint16_t)sizeof(Params));
  prefs.putBytes("params", &P, sizeof(Params));
  prefs.putUChar("amb", ambIndex);
  prefs.end();
}

static void loadParams() {
  prefs.begin(NVS_NS, true);
  uint16_t ver  = prefs.getUShort("pver", 0);
  uint16_t size = prefs.getUShort("psize", 0);
  // Stored as one blob rather than a key per parameter: twenty-odd names would
  // drift out of step with the struct the first time one was renamed, and a
  // silently-missing key reads as a value of zero — which for `emberFloor` or
  // `breathHz` is a dead-looking fire rather than an error.
  //
  // The version AND size must both match. If either differs, the firmware's
  // parameter set has changed since the save, and the stored bytes mean
  // something else now. Ignoring them is correct: compiled defaults are known
  // good, and reinterpreting an old struct is how you get a fire whose values
  // are individually plausible and collectively wrong.
  if (ver == PARAMS_VERSION && size == sizeof(Params)) {
    Params tmp;
    if (prefs.getBytes("params", &tmp, sizeof(tmp)) == sizeof(tmp)) {
      P = tmp;
      // The one loaded value that can index an array. Version and size agree,
      // so this cannot be a stale blob — but MAX_LEDS is a compile-time number
      // and someone LOWERING it would leave a legitimately-saved length
      // pointing past the end of `leds`, which is a memory fault rather than a
      // wrong-looking fire. Cheap, and it fails toward the shorter bar.
      if (P.numLeds < 1)        P.numLeds = 1;
      if (P.numLeds > MAX_LEDS) P.numLeds = MAX_LEDS;
      uint8_t a = prefs.getUChar("amb", 0);
      if (a < N_AMBIENCE) ambIndex = a;
    }
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
  ambIndex = 0;
  FastLED.setBrightness(P.brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, P.maxMA);
  buildGamma();
}

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

// `ends` paints the first and last pixel of the CURRENT length and nothing
// between, for a fixed spell. It is the only way to answer "does `numLeds`
// match the strip?" without a toolchain — and it has to be a mode rather than
// a one-shot draw, because the render loop repaints every pixel 100 times a
// second and would wipe a single frame before anyone saw it.
static uint32_t endsUntilMs = 0;
static const uint32_t ENDS_MS = 15000;

static void spawnFlare(float amp, float atPos = -1.0f) {
  for (uint8_t f = 0; f < MAX_FLARES; f++) {
    if (flares[f].live) continue;
    flares[f] = {true,
                 atPos >= 0 ? atPos : (random16() / 65535.0f) * (P.numLeds - 1),
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

// The track on flash. A tiny header so a file from an older build, or a
// half-written one, is rejected rather than rendered as noise.
static const char*    TRACK_PATH  = "/track.bin";
static const uint32_t TRACK_MAGIC = 0x4B525442;        // "BTRK"
static const uint16_t TRACK_VER   = 1;

struct TrackHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t clip;
  uint16_t frames;
};

static bool fsReady = false;

static bool saveTrack() {
  if (!fsReady || !trackFrames) return false;
  File f = LittleFS.open(TRACK_PATH, "w");
  if (!f) return false;
  TrackHeader h{TRACK_MAGIC, TRACK_VER, trackClip, trackFrames};
  bool ok = f.write((uint8_t*)&h, sizeof(h)) == sizeof(h);
  if (ok) {
    size_t n = (size_t)trackFrames * 2;
    ok = f.write(trackBuf, n) == n;
  }
  f.close();
  if (!ok) LittleFS.remove(TRACK_PATH);   // never leave half a track behind
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
    if (ok) {
      trackFrames = h.frames;
      trackClip   = h.clip;
    }
  }
  f.close();
  return ok;
}
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
  // The trailing partial group. THIS IS NOT OPTIONAL, and leaving it out is a
  // bug that hides: a chunk whose length is a multiple of 3 encodes with no
  // padding and decodes exactly, so it works until it doesn't.
  //
  // It cost a whole diagnosis. A 4134-byte envelope ends in an 84-byte chunk —
  // divisible by 3, no padding — and loaded perfectly. A 4136-byte one ends in
  // 86 bytes, which pads, and silently arrived 2 bytes short: `trk err short`,
  // on a transport that had just been proven working with a different track.
  if (q == 2) {                        // 2 chars -> 1 byte
    if (n < outMax) out[n++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
  } else if (q == 3) {                 // 3 chars -> 2 bytes
    if (n < outMax) out[n++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    if (n < outMax) out[n++] = (uint8_t)((quad[1] << 4) | (quad[2] >> 2));
  }
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
// Red is ON for the DURATION of a word and off between words — driven by the
// track's LEVEL channel, not its onset. Triggering on the attack and decaying on
// a timer is a hit, and reads as flashing; a word is a span, and lighting the
// span reads as the word being spoken.
//
// It gets its own release rather than borrowing the fire's thermal inertia,
// because the two want opposite things: the fire should settle slowly, the red
// should leave cleanly so the gaps between words are genuinely dark.
static float flashLevel = 0.0f;   // 0..1, follows the word span
static float trackLevel = 0.0f;   // raw level from the track, this frame
// 1 while a gap is short enough to punctuate, easing to 0 across a long silence.
static float gapScale = 1.0f;
// True while speech is happening from ANY source — a cued track or the synthetic
// stand-in. Everything speech-dependent keys off this rather than off `playing`.
//
// It exists because `speak` kept turning out to be an unfaithful stand-in, twice
// in one afternoon. First it drove the heat but never `trackLevel`, so no red
// was ever mixed and four parameters — flash, flashHue, flashDecay, blackout —
// could not be judged with it at all. Then the blackout turned out to be gated
// on `playing`, so the stand-in could not exercise that either. Each gap cost a
// 45s greeting and a walk past a fog machine to discover. A test rig that is
// quietly missing the thing under test is worse than no test rig.
static bool speechActive = false;
// Mid-strip heat, kept only so `st` can report it. Cheap, and it separates "the
// palette lookup is wrong" from "heat never moved".
static float lastHeatMid = 0.0f;
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
// MEASURED against the real Tiki greeting, 2026-09-10:
//
//     gate 0.02 -> 17.9 flares/sec      gate 0.16 ->  7.6/sec
//     gate 0.06 -> 13.5 flares/sec      gate 0.25 ->  5.5/sec
//     words in that greeting            ->  1.28/sec
//
// So even a punishing gate asks for four times more crackles than there are
// words, and 0.06 asks for ten times. That is not a fire responding to speech,
// it is flicker — which is the same conclusion the Zoltar build reached from the
// other end, where a servo jaw had to be driven from word starts rather than
// syllables. There it was a mechanical limit; here there is no load at all and
// the perceptual limit lands in the same place.
//
// The gate alone cannot fix it: the onset channel genuinely has that much
// structure. What caps the RATE is the refractory below. The real answer is to
// spawn flares from word starts and leave the envelope to drive the glow —
// docs/01 §4's "use your word timestamps for structure" — and this holds the
// line until that track exists.

static void feedSpeech(float dt) {
  float raw = 0.0f;

  // Priority: a manual hold, then a real track, then the synthetic stand-in.
  // The track wins over `speak` so a cue arriving mid-demo does the right thing.
  trackLevel = 0.0f;
  if (envHold >= 0.0f) {
    raw = envHold;
    trackLevel = envHold;
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
        trackLevel = raw;                 // red follows this, not the onset
        float on = trackBuf[f * 2 + 1] / 255.0f;
        // Gate, then rate-limit. A refractory period is what actually stops the
        // flicker — see the note at ONSET_GATE. 0.28s caps this at ~3.5/sec
        // against a word rate of ~1.3/sec, so the loudest attack in each word
        // wins and the syllables inside it do not each get their own crackle.
        static uint32_t lastFlareMs = 0;
        if (P.wordFlare > 0.0f && on > P.onsetGate &&
            millis() - lastFlareMs > (uint32_t)(P.flareGap * 1000.0f)) {
          lastFlareMs = millis();
          spawnFlare(fminf(1.0f, on * 1.4f * P.wordFlare));
        }
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
      // Drive the WORD COLOUR too, not just the heat.
      //
      // This was missing, and it quietly made the stand-in useless for the one
      // thing it exists for. `trackLevel` is what `flashLevel` follows, and it
      // was set in the held-env branch and the real-track branch but not here —
      // so `speak` lifted the fire's heat and never mixed in a single frame of
      // red. The effect under `speak` was "slightly brighter fire", which is
      // exactly what Steve reported when a 25s stand-in showed hardly any
      // difference from idle.
      //
      // It matters beyond the confusion: `speak` is the loop this renderer is
      // meant to be tuned in, and `flash`, `flashHue`, `flashDecay` and
      // `blackout` are all downstream of `trackLevel`. Four of the parameters
      // could never be judged with it, so they were only ever judged against a
      // real greeting — at 45 seconds and a walk-past per attempt.
      trackLevel = raw;
    }
  } else if (speakUntil && millis() >= speakUntil) {
    speakUntil = 0;
    Serial.println(F("speak: done"));
  }

  // Set before the smoothing below, so everything downstream this frame agrees
  // about whether speech is happening.
  speechActive = playing || (envHold >= 0.0f) ||
                 (speakUntil && millis() < speakUntil);

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

// FastLED's inoise8 does NOT use its full range. In practice it clusters around
// 128 and rarely leaves roughly 50..205, so treating the raw byte as 0..1 gives
// you about a third of the dynamic range you think you have:
//
//     breath  was swinging 0.81..0.94  — a 13% wobble, not a breath
//     noise   was running  0.24..0.75  — never reaching the top of the palette
//
// That is why the fire did not look like it was breathing, and it is very
// probably why the palette read as too red earlier: heat never got near the
// bright end, so every judgement about the ramp was made from its bottom third.
// Stretch it back out before anything else uses it.
static const float NOISE_LO = 50.0f;
static const float NOISE_HI = 205.0f;

static float noiseNorm(uint8_t raw) {
  float v = (raw - NOISE_LO) / (NOISE_HI - NOISE_LO);
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float heat[MAX_LEDS];

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

  // Capture the compiled values BEFORE anything is restored over them, so
  // `forget` has somewhere to go back to.
  DEFAULTS = P;

  // Restore before FastLED is told the brightness, or a saved brightness would
  // be set and then immediately overwritten by the compiled one.
  loadParams();
  buildLampPalette();          // from the restored hue/sat
  fsReady = LittleFS.begin(true);          // format on first boot
  bool restored = loadTrack();

  // MAX_LEDS, not `P.numLeds`: FastLED controllers can be added but not
  // removed or resized, so binding the live length here would make `set leds`
  // a reboot rather than a knob. The whole buffer is clocked out and the tail
  // is held black by the render loop instead.
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, MAX_LEDS)
         .setCorrection(TypicalLEDStrip);
  // From P, not from the define: `loadParams()` has already run, so a saved cap
  // must win here the same way a saved brightness does.
  FastLED.setMaxPowerInVoltsAndMilliamps(5, P.maxMA);
  FastLED.setBrightness(P.brightness);
  buildGamma();
  random16_set_seed((uint16_t)esp_random());
  linkBegin();

  Serial.println();
  Serial.println(F("=== light bar — ambience renderer ========================"));
  Serial.print  (F("ambience : ")); Serial.println(AMBIENCES[ambIndex].name);
  Serial.print  (F("storage  : "));
  if (!fsReady) {
    Serial.println(F("LittleFS UNAVAILABLE — a power cycle will lose the track"));
  } else if (restored) {
    Serial.printf("track restored, clip %u, %u frames\n", trackClip, trackFrames);
  } else {
    Serial.println(F("ready, no stored track"));
  }
  Serial.print  (F("pixels   : ")); Serial.print(P.numLeds);
  Serial.print  (F(" of ")); Serial.print(MAX_LEDS);
  Serial.print  (F(" max = ")); Serial.print(P.numLeds / 144.0f * 39.37f, 1);
  Serial.print  (F("\" (")); Serial.print(P.numLeds / 144.0f, 2);
  Serial.println(F(" m at 144/m) — `len <inches>` or `set leds <n>`"));
  Serial.print  (F("LED cap  : ")); Serial.print(P.maxMA);
  Serial.println(F(" mA at 5 V — `pwr` for the estimate against a meter"));
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
  Serial.print(F("onsetGate     ")); Serial.println(P.onsetGate, 3);
  Serial.print(F("flash         ")); Serial.println(P.flash, 3);
  Serial.print(F("flashDecay    ")); Serial.println(P.flashDecay, 3);
  Serial.print(F("wordFlare     ")); Serial.println(P.wordFlare, 3);
  Serial.print(F("flashHue      ")); Serial.println(P.flashHue, 0);
  Serial.print(F("blackout      ")); Serial.println(P.blackout, 2);
  Serial.print(F("flareGap      ")); Serial.println(P.flareGap, 3);
  Serial.print(F("gamma         ")); Serial.println(P.gamma, 2);
  Serial.print(F("brightness    ")); Serial.println(P.brightness);
  Serial.print(F("maxMA         ")); Serial.println(P.maxMA);
  Serial.print(F("leds          ")); Serial.println(P.numLeds);
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
  else if (k == "onsetGate")    P.onsetGate    = v;
  else if (k == "flash")        P.flash        = v;
  else if (k == "flashDecay")   P.flashDecay   = v;
  else if (k == "wordFlare")    P.wordFlare    = v;
  else if (k == "flashHue")     P.flashHue     = v;
  else if (k == "blackout")     P.blackout     = v;
  else if (k == "blackoutHold") P.blackoutHold = v;
  else if (k == "blackoutEase") P.blackoutEase = v;
  else if (k == "blinkHz")      P.blinkHz      = v;
  else if (k == "blinkDuty")    P.blinkDuty    = v;
  else if (k == "lampHue")    { P.lampHue = (uint8_t)v; buildLampPalette(); }
  else if (k == "lampSat")    { P.lampSat = (uint8_t)v; buildLampPalette(); }
  else if (k == "flareGap")     P.flareGap     = v;
  else if (k == "gamma")      { P.gamma = v; buildGamma(); }
  else if (k == "brightness") { P.brightness = (uint8_t)v;
                                FastLED.setBrightness(P.brightness); }
  else if (k == "maxMA" || k == "mA") {
                                P.maxMA = (uint16_t)v;
                                FastLED.setMaxPowerInVoltsAndMilliamps(5, P.maxMA); }
  // Clamped rather than rejected: the useful failure is asking for more strip
  // than this build can drive and getting the most it can, with `show`/`st`
  // saying what you actually got. Zero would divide by zero in the ends check
  // and render nothing, so the floor is 1.
  else if (k == "leds" || k == "numLeds") {
                                if (v < 1) v = 1;
                                if (v > MAX_LEDS) v = MAX_LEDS;
                                P.numLeds = (uint16_t)v; }
  else return false;
  return true;
}

// ---------------------------------------------------------------------------
// WHAT IT IS ACTUALLY DRAWING — estimated, and reported so that a meter has
// something to disagree with.
//
// At 50 pixels this question never arose: the frame never came near the cap, so
// the limiter was inert and `brightness` was the whole story. At 144 the cap is
// close enough to the truth that "is the limiter dimming me?" is a question
// about the LOOK and not only about the battery, and it cannot be answered by
// reading the source — it depends on the frame on the strip right now.
//
// Three numbers, because two of them are routinely confused:
//
//   wantMA   what this frame would draw at `brightness`, uncapped
//   capMA    the ceiling in force
//   estMA    what FastLED will actually let it draw
//
// estMA < wantMA means the limiter is holding the frame down. FastLED's own
// model is used rather than a constant per pixel, so the estimate tracks the
// fire instead of assuming white — but it is a MODEL: it counts the LEDs only,
// at an assumed 5.0 V, and excludes the ~40-50 mA the XIAO itself takes.
//
// MEASURED AGAINST A METER, 2026-09-12: it reads BELOW this estimate, not above,
// which is the opposite of what this comment first predicted. Supply 5.05 V /
// ~0.80 A total, against an estMA of 0.89-1.19 A for the LEDs alone — so with
// the XIAO's ~45 mA backed out, the model reads high by roughly a fifth to a
// third. The cause is `setCorrection(TypicalLEDStrip)`: it scales what actually
// reaches the pixels, and `calculate_unscaled_power_mW` does not know about it.
//
// So the estimate errs HIGH, which is the safe direction for a cap — a real
// frame is further from the limit than this claims, never closer. A meter far
// ABOVE it would mean the model's assumptions are wrong, and then the meter wins.
// ---------------------------------------------------------------------------
// Outputs by reference rather than a returned struct: the .ino preprocessor
// generates prototypes ahead of every file-scope type, so a function returning
// one declared here does not compile.
static void estimatePower(uint32_t& wantMA, uint32_t& estMA, uint8_t& allowed) {
  // MAX_LEDS and not `P.numLeds`, because the whole buffer is what gets
  // clocked out — and it costs nothing to be right about it: the tail is black
  // every frame, and black pixels contribute only their quiescent draw to
  // FastLED's model. A shortened bar therefore reports a proportionally
  // smaller number here without any arithmetic of ours.
  uint32_t unscaled = calculate_unscaled_power_mW(leds, MAX_LEDS);
  allowed = calculate_max_brightness_for_power_vmA(
              leds, MAX_LEDS, P.brightness, 5, P.maxMA);
  // mW at 5 V -> mA, after the brightness scale each case implies. 144 white
  // pixels is ~43 W, so 43000 * 255 still sits well inside a uint32.
  wantMA = (unscaled * P.brightness) / 255 / 5;
  estMA  = (unscaled * allowed)      / 255 / 5;
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
    uint32_t pwrWant, pwrEst; uint8_t pwrAllowed;
    estimatePower(pwrWant, pwrEst, pwrAllowed);
    String out = "st amb=" + String(usingLamp ? "lamp"
                                             : AMBIENCES[ambIndex].name)
               + " motion=" + String(MOTION_NAMES[P.motion])
               + " hue=" + String(P.lampHue)
               + " y=" + String(P.yellow, 2)
               + " br=" + String(P.brightness)
               // The draw, because at 144 pixels it is state and not trivia.
               // Two tokens rather than a flagged one: the box parses `st` by
               // splitting on `=`, so `mA=1487!` would hand it a number that
               // is not one. `capped=1` says the current cap — not `br` — set
               // the level of the frame being looked at; `pwr` breaks it down.
               + " mA=" + String(pwrEst)
               + (pwrAllowed < P.brightness ? " capped=1" : "")
               // The bar's LENGTH, which is now a setting and so is now state.
               // `leds=` and not `px=`: `px=` already means the middle pixel's
               // colour six tokens down, and one key meaning two things in one
               // line is how a parser that splits on `=` gets quietly wrong.
               + " leds=" + String(P.numLeds)
               + " breath=" + String(P.breathHz, 2)
               + " rel=" + String(P.release, 3)
               + " gain=" + String(P.speechGain, 2)
               + " env=" + String(env, 2)
               // The word-colour path, reported because it cannot be inferred.
               // An afternoon went into "is it the red or the fire?" with no way
               // to ask: `st` carried env (the HEAT path) and nothing about the
               // colour path at all, so every question about the red was
               // answered by reading the source and guessing. These two are the
               // whole chain — trackLevel is what the box asked for, flashLevel
               // is what the renderer is acting on, and `mix` is what actually
               // reaches a pixel.
               + " trkLvl=" + String(trackLevel, 2)
               + " flash=" + String(flashLevel, 2)
               + " mix=" + String(fminf(1.0f, flashLevel * P.flash), 2)
               // The LAST link in the chain: what actually reached a pixel.
               // Everything above is what the renderer COMPUTED, and an
               // afternoon was spent arguing about the difference. If mix moves
               // and this does not, the fault is between the maths and the
               // strip — not in the track, the envelope or the parameters.
               + " px=" + String(leds[P.numLeds / 2].r) + ","
                        + String(leds[P.numLeds / 2].g) + ","
                        + String(leds[P.numLeds / 2].b)
               + " heat=" + String(lastHeatMid, 2)
               + " trk=" + String(trackFrames)
               // The CLIP, not just the count. The box verifies what the
               // renderer holds rather than believing its own record — and once
               // a track survives a reboot, "something is loaded" stops being
               // the same question as "the right thing is loaded". A restored
               // but stale track would otherwise satisfy that check and the
               // cue would be refused with nothing having warned anyone.
               + " clip=" + String(trackClip)
               + (playing ? " playing@" + String(trackPosMs()) : " idle");
    Serial.println(out);
    linkSend(out);
    return;
  }

  // Answers over the radio, and for exactly the reason `st` does: the board is
  // never interrupted that way, so this reports the frame that is on the strip
  // rather than the first frame after a reboot.
  if (verb == "pwr") {
    uint32_t wantMA, estMA; uint8_t allowed;
    estimatePower(wantMA, estMA, allowed);
    String out = "pwr leds=" + String(P.numLeds)
               + " br=" + String(P.brightness)
               + " allowed=" + String(allowed)
               + " wantMA=" + String(wantMA)
               + " capMA=" + String(P.maxMA)
               + " estMA=" + String(estMA)
               + (allowed < P.brightness ? " LIMITING" : " headroom")
               + " (+~45mA XIAO, not counted)";
    Serial.println(out);
    linkSend(out);
    return;
  }

  // Length in INCHES, because that is the unit the bar is cut and mounted in
  // and 144 LED/m is not a number anyone should have to divide by. It sets the
  // same value `set leds` does — this is a convenience, not a second source of
  // truth — and it answers with both, so the conversion is never a guess.
  if (verb == "len") {
    if (rest.length()) {
      float inches = rest.toFloat();
      // 144 LED/m over 39.37 in/m = 3.6576 pixels per inch.
      float px = inches * 144.0f / 39.37f;
      setParam("leds", px);            // clamps, and owns the rule
    }
    String out = "len " + String(P.numLeds / 144.0f * 39.37f, 1) + "in leds="
               + String(P.numLeds) + " max=" + String(MAX_LEDS);
    Serial.println(out);
    linkSend(out);
    return;
  }

  // Does the length match the strip? Nothing else in the firmware can tell.
  if (verb == "ends") {
    endsUntilMs = millis() + ENDS_MS;
    String out = "ends 0 and " + String(P.numLeds - 1) + " lit for "
               + String(ENDS_MS / 1000) + "s — the far marker should be the "
                 "last pixel of the bar";
    Serial.println(out);
    linkSend(out);
    return;
  }

  // Explicit, not automatic. Tuning is dozens of nudges and each one would be a
  // flash write; and being explicit matches how a look is actually arrived at —
  // try a value, watch the strip, decide.
  if (verb == "save") {
    saveParams();
    String m = "saved params" + String(trackFrames ? " (track already on flash)" : "");
    Serial.println(m);
    linkSend(m);
    return;
  }

  if (verb == "forget") {
    clearParams();
    if (fsReady) LittleFS.remove(TRACK_PATH);
    trackFrames = 0;
    trackClip = 0;
    playing = false;
    String m = "forgot everything — compiled defaults, no track";
    Serial.println(m);
    linkSend(m);
    return;
  }

  if (verb == "help") {
    Serial.println(F("show                 every live parameter (serial only)"));
    Serial.println(F("st                   one-line state, answers over the radio"));
    Serial.println(F("pwr                  estimated draw vs the cap, over the radio"));
    Serial.println(F("set <key> <value>    change one (see show for keys)"));
    Serial.println(F("amb <fire|water|storm|lamp>"));
    Serial.println(F("lamp <hue> [sat]     a plain colour, and select it"));
    Serial.println(F("motion <ambient|solid|blink|breathe>"));
    Serial.println(F("speak <seconds>      synthetic speech envelope"));
    Serial.println(F("env <0..1>           hold excitation; 'idle' releases"));
    Serial.println(F("set yellow 0.8       redder | 1.3 more yellow"));
    Serial.println(F("set brightness 70    less light reads as MORE saturated"));
    Serial.println(F("set maxMA 1500       LED current cap; 1700 is the supply's limit"));
    Serial.println(F("set leds <n>         how many pixels the bar actually is"));
    Serial.println(F("len [inches]         the same, in inches; no arg just asks"));
    Serial.println(F("ends                 light pixel 0 and the last one, 15s"));
    Serial.println(F("idle                 release the hold, stop speaking"));
    Serial.println(F("flare                fire one crackle now"));
    Serial.println(F("trk begin|d|end      load an envelope track"));
    Serial.println(F("cue <clip> <ms> <lead>   start it, on the box's clock"));
    Serial.println(F("pos <ms>             correction; eased, never jumped"));
    Serial.println(F("stop                 release to idle"));
    Serial.println(F("save                 keep these params across a power cycle"));
    Serial.println(F("forget               back to compiled defaults, drop the track"));
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

  if (verb == "lamp") {
    // `lamp <hue> [sat]` — and it selects the lamp, because asking for a colour
    // and then having to select it separately is a step nobody would want.
    int sp2 = rest.indexOf(' ');
    String hs = (sp2 < 0) ? rest : rest.substring(0, sp2);
    if (!hs.length()) {
      String e = "lamp <hue 0-255> [sat 0-255]";
      Serial.println(e); linkSend(e); return;
    }
    P.lampHue = (uint8_t)constrain(hs.toInt(), 0, 255);
    if (sp2 >= 0) P.lampSat = (uint8_t)constrain(rest.substring(sp2 + 1).toInt(),
                                                 0, 255);
    usingLamp = true;
    buildLampPalette();
    String m = "lamp hue " + String(P.lampHue) + " sat " + String(P.lampSat);
    Serial.println(m); linkSend(m);
    return;
  }

  if (verb == "motion") {
    for (uint8_t i = 0; i < N_MOTION; i++) {
      if (rest == MOTION_NAMES[i]) {
        P.motion = i;
        String m = "motion " + rest;
        Serial.println(m); linkSend(m);
        return;
      }
    }
    String e = "motion must be ambient, solid, blink or breathe";
    Serial.println(e); linkSend(e);
    return;
  }

  if (verb == "amb") {
    if (rest == "lamp") {
      usingLamp = true;
      buildLampPalette();
      Serial.println(F("ambience = lamp"));
      return;
    }
    for (uint8_t i = 0; i < N_AMBIENCE; i++) {
      if (rest == AMBIENCES[i].name) {
        ambIndex = i;
        usingLamp = false;
        palette = AMBIENCES[i].pal;
        Serial.println("ambience = " + rest);
        return;
      }
    }
    Serial.println(F("ambience must be fire, water, storm or lamp"));
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
      // Straight to flash. The box pushes a track rarely and never during a
      // show, so there is no wear question and nothing to defer — and a track
      // that survives a power cycle is the entire point of this being here.
      bool saved = saveTrack();
      String m = "trk ok " + String(trackClip) + " " + String(trackFrames)
               + (saved ? " saved" : " (not saved)");
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
    // Echo the verb, not the payload. A track chunk is ~215 characters and
    // there are 28 of them; echoing the lot is 6 kB of USB traffic during the
    // one operation that must not be interrupted.
    if (line.startsWith("trk d ")) {
      Serial.print(F("rf> ")); Serial.println(line.substring(0, 12));
    } else {
      Serial.print(F("rf> ")); Serial.println(line);
    }
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

  // Red tracks the word span: on almost immediately, off on its own release.
  // Asymmetric for the same reason the envelope is — a word should light without
  // a visible ramp, and leave without a smear into the gap.
  if (trackLevel > flashLevel) {
    flashLevel += (trackLevel - flashLevel) * 0.6f;
  } else {
    float k = 1.0f - expf(-dt / fmaxf(0.02f, P.flashDecay));
    flashLevel += (trackLevel - flashLevel) * k;
  }
  if (flashLevel < 0.004f) flashLevel = 0.0f;

  // How long the track has been saying nothing, and hence whether this is a
  // PAUSE (punctuate it) or a SILENCE (fill it). Measured from trackLevel
  // rather than flashLevel so the renderer's own release does not extend it.
  static float gapSec = 0.0f;
  if (trackLevel > 0.01f) gapSec = 0.0f;
  else                    gapSec += dt;
  gapScale = 1.0f;
  if (speechActive && gapSec > P.blackoutHold) {
    float over = (gapSec - P.blackoutHold) / fmaxf(0.05f, P.blackoutEase);
    gapScale = over >= 1.0f ? 0.0f : (1.0f - over);
  }

  // ------------------------------------------------------------------------
  // NOISE PHASE IS ACCUMULATED, NEVER `time * rate`. This is load-bearing.
  //
  // It used to read:
  //
  //     uint16_t tz = (uint16_t)(tSec * P.timeScale * (1.0f + 0.6f * env));
  //
  // which multiplies ABSOLUTE time by a rate that speech is modulating. The
  // moment `env` moves, the product jumps: thirty seconds in, env going
  // 0.4 -> 0.9 shifts tz by over a THOUSAND noise units instantly, so the
  // pattern does not speed up — it teleports. With a real speech envelope
  // moving several times a second, the whole strip scrubs back and forth and
  // reads as rapid blinking.
  //
  // Found on the prop, 2026-09-11, and it had survived every previous
  // judgement of this renderer because of how those judgements were made: a
  // HELD `env` renders perfectly (constant rate, no jumps), and the synthetic
  // `speak` was only ever watched for a few seconds. The bug needs a
  // fast-moving envelope to show itself, which only a real track provides.
  // Steve spent an afternoon reporting it before I stopped adjusting the track
  // and bisected the renderer: held env = calm, any playing track = blinking,
  // frozen noise field = calm again.
  //
  // Integrating the rate instead is the whole fix. Speech now makes the fire
  // drift genuinely faster, continuously, with no discontinuity anywhere.
  static float noisePhase = 0.0f;
  static float breathPhase = 0.0f;
  float timeScale = P.timeScale * (1.0f + 0.6f * env);
  noisePhase  += dt * timeScale;
  // The breath integrates too. `breathHz` is not modulated by speech, so this
  // was not the reported fault — but `set breathHz` while running is exactly
  // the same discontinuity, and a tuning slider that jumps the pattern each
  // time it moves makes the value impossible to judge.
  breathPhase += dt * P.breathHz * 256.0f;

  // Layer 1 — breath. One value for the whole strip. Low-frequency noise rather
  // than a sine, because a periodic breath is audible to the eye as a loop.
  uint16_t bz = (uint16_t)breathPhase;
  float breathRaw = noiseNorm(inoise8(0, bz));
  float breath = (1.0f - P.breathDepth) + P.breathDepth * breathRaw;

  // Speech widens the zones as well as brightening them — but note that this
  // one CANNOT be fixed by integration, because it scales a SPATIAL coordinate
  // rather than a rate: changing it rescales the pattern under the viewer, which
  // is a zoom, and a zoom driven by a speech envelope is the same visual fault
  // as the jump above. Hence `speechSpread` now defaults to 0. It is kept
  // because a slow envelope can use it safely, and removing a parameter people
  // have tuned against is worse than documenting its edge.
  float spaceScale = P.spaceScale * (1.0f - P.speechSpread * 0.5f * env);

  // Layers 3 — advance the crackles.
  //
  // The pixel loops below index with uint16_t, not uint8_t. 144 still fits in a
  // byte so this build works either way, but `i < P.numLeds` with a uint8_t `i`
  // does not merely truncate at 256 — it never terminates, and it would do that
  // the first time someone added a second metre of strip. Cheap insurance
  // against a change that otherwise looks like a one-line edit.
  //
  // MAX_LEDS floats is 576 B of stack per frame at 144, which the Arduino loop
  // task's 8 K absorbs; a MAX_LEDS several times larger should move this to a
  // static buffer like `heat` already is. Sized by the ceiling rather than by
  // `numLeds` because a variable-length array would re-cost this every frame
  // and buy nothing — the loops below stop at `numLeds` regardless.
  float flareField[MAX_LEDS] = {0};
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
    for (uint16_t i = 0; i < P.numLeds; i++) {
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
  // The blink phase is ACCUMULATED, for the same reason the noise phase is:
  // `time * blinkHz` jumps the moment the rate changes, so adjusting the rate
  // while watching would make the lamp stutter rather than slow down. That bug
  // cost an afternoon on the noise field; it is not going in a second time.
  static float blinkPhase = 0.0f;
  blinkPhase += dt * P.blinkHz;
  if (blinkPhase >= 1.0f) blinkPhase -= (float)(int)blinkPhase;

  uint16_t tz = (uint16_t)noisePhase;
  for (uint16_t i = 0; i < P.numLeds; i++) {
    // `motion` decides what feeds heat; everything after this is unchanged, so
    // a lamp goes through the same palette, the same speech excitation, the same
    // word colour and the same blackout as a fire does. That is the point of
    // doing it here rather than as a second renderer.
    float base;
    switch (P.motion) {
      case MOTION_SOLID:
        base = 1.0f;
        break;
      case MOTION_BLINK:
        base = (blinkPhase < P.blinkDuty) ? 1.0f : 0.0f;
        break;
      case MOTION_BREATHE:
        // The breath alone — one value for the whole strip, no spatial noise
        // and no crackles. A lamp that breathes should swell as a single light,
        // not shimmer along its length.
        base = breath;
        break;
      default:
        base = (noiseNorm(inoise8((uint16_t)(i * spaceScale), tz))
                + flareField[i]) * breath;
        break;
    }
    float h = base * (1.0f + P.speechGain * env);
    // The ember floor is an AMBIENCE rule — "for a fire, dark reads as broken".
    // A lamp must be able to be off, or blinking has nothing to blink to.
    float floorHere = (P.motion == MOTION_AMBIENT) ? P.emberFloor : 0.0f;
    heat[i] = h < floorHere ? floorHere : (h > 1.0f ? 1.0f : h);
  }
  lastHeatMid = heat[P.numLeds / 2];

  // Colour is a function of heat and nothing else, then gamma last.
  for (uint16_t i = 0; i < P.numLeds; i++) {
    // LINEARBLEND_NOWRAP, and the difference is not cosmetic.
    //
    // Plain LINEARBLEND treats a 16-entry palette as a RING: at index 255 it
    // blends entry 15 back toward entry 0, so it returns 15/16 of entry 0 —
    // which for every palette here is BLACK. Heat clamps to 1.0, so heat 1.0
    // rendered as almost nothing.
    //
    // Found when `motion solid` pinned heat at exactly 1.0 and the strip went
    // dark: "the light bar is not on anymore". But the fire had it too, and
    // silently — peak heat happens on the loudest syllables, so the brightest
    // moments of speech were dropping individual pixels to black. That is a
    // stray dark flash exactly where the effect is meant to be strongest, and
    // it is very likely part of what read as flicker for an afternoon.
    //
    // NOWRAP holds entry 15 at the top instead of wrapping. FastLED provides it
    // for precisely this case.
    CRGB c = ColorFromPalette(palette, (uint8_t)(heat[i] * 255.0f), 255,
                              LINEARBLEND_NOWRAP);
    // Blend toward a bright saturated red for the word flash. Applied before
    // the yellow trim and the gamma table so it goes through the same output
    // path as everything else rather than becoming a second way to write a pixel.
    if (flashLevel > 0.01f && P.flash > 0.0f) {
      uint8_t mix = (uint8_t)(fminf(1.0f, flashLevel * P.flash) * 255.0f);
      // Full saturation and value: the point is contrast against the bed, and a
      // desaturated speech colour just reads as the fire getting paler.
      c = blend(c, CRGB(CHSV((uint8_t)P.flashHue, 255, 255)), mix);
    }
    // Everything off between words, while and only while a track is playing.
    // Applied last, as a master scale, so it dims the fire and the speech colour
    // together rather than becoming a second way to decide what a pixel is.
    //
    // `gapScale` is what distinguishes a pause from a silence — see the note on
    // `blackoutHold`. A short gap stays dark and punctuates the speech; a long
    // one gives the ambience back, so a musical introduction burns normally.
    if (speechActive && P.blackout > 0.0f) {
      float keep = 1.0f - P.blackout * gapScale * (1.0f - flashLevel);
      if (keep < 0.0f) keep = 0.0f;
      c.nscale8_video((uint8_t)(keep * 255.0f));
    }
    if (P.yellow != 1.0f) {
      float g = c.g * P.yellow;
      c.g = (uint8_t)(g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g));
    }
    leds[i] = CRGB(gammaLUT[c.r], gammaLUT[c.g], gammaLUT[c.b]);
  }

  // The tail beyond the bar's length, held black EVERY frame rather than once
  // when `numLeds` changes. FastLED clocks the whole buffer whatever we do, so
  // a tail written once and then left alone would freeze the last image on it —
  // which on a strip physically longer than `numLeds` is a stripe of dead fire
  // past the end of the bar, and on a shorter one is invisible and therefore
  // never debugged. Doing it here costs a memset of at most MAX_LEDS pixels.
  for (uint16_t i = P.numLeds; i < MAX_LEDS; i++) leds[i] = CRGB::Black;

  // `ends` overrides everything above, and does so AFTER the frame is built so
  // it cannot be defeated by an ambience, a lamp, a blackout or a track. Two
  // pixels, deliberately dim and different colours: the question is *where the
  // far one is*, and a bright white marker on a diffuser blooms enough to move
  // the answer by an inch.
  if (endsUntilMs) {
    if ((int32_t)(millis() - endsUntilMs) >= 0) {
      endsUntilMs = 0;                 // spell over; the next frame is normal
    } else {
      for (uint16_t i = 0; i < MAX_LEDS; i++) leds[i] = CRGB::Black;
      leds[0] = CRGB(60, 30, 0);                    // amber: the near end
      leds[P.numLeds - 1] = CRGB(0, 20, 60);        // blue:  the far end
    }
  }

  FastLED.show();
}
