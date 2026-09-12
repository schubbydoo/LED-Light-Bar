/*
  Bring-up — the first thing that ever runs on this board.

  Nothing downstream of the XIAO's D10 pin has been exercised. The strip was
  smoke-tested on its bundled SP002E controller, so the pixels and the power path
  are known good; what has never been proven is *our* signal reaching them —
  D10, the AHCT125 buffer, the 470R, the JST.

  So this is deliberately not the fire renderer. It is five questions asked in
  order, each one narrowing where a fault can be, with the serial monitor saying
  what you should be seeing. Run it, watch, and read the two lists at the bottom
  of this comment.

      1  PIXEL ZERO    one pixel, dim red        is the signal getting out at all?
      2  COLOUR ORDER  red, then green, then blue  is the byte order really GRB?
      3  WALK          one pixel travelling       are all 144 addressable?
      4  THE ENDS      first and last only        does NUM_LEDS match the strip?
      5  LOAD          the whole strip, amber     does the supply hold up?

  Then it repeats, so you can leave it running while you probe.

  ---------------------------------------------------------------------------
  IF STAGE 1 SHOWS NOTHING

  In order of likelihood, and the first one is not a guess — it is the specific
  way this circuit fails while looking perfect:

    1  Pin 1 (1OE) of the AHCT125 is not actually at GND. The buffer's output
       sits high-impedance, the strip sees nothing, and every voltage you meter
       is correct. Meter pin 1 to GND directly; do not trust that it looks wired.
    2  The strip has no power. USB alone powers the XIAO but not 144 pixels — the
       1N5817 is there to stop the PC trying. If the TalentCell is not connected,
       a perfectly good sketch looks like a dead board.
    3  DIN and +5V swapped at the strip. +5V on the DIN pad kills pixel 0
       instantly, and this stage lights only pixel 0. Try stage 3 (the walk): if
       pixels 1..143 work and 0 never does, that is your answer.
    4  Data on the wrong pin. D10 is GPIO10 on the silkscreen. Note that the
       XIAO's silkscreen is on its UNDERSIDE.

  IF THE COLOURS ARE WRONG AT STAGE 2

  Change COLOR_ORDER below and reflash. GRB is FastLED's default for WS2812B and
  is what this strip is believed to be, but it is worth one look rather than an
  assumption carried into the fire palette, where a swapped red and green would
  read as "the fire model is wrong" rather than "the byte order is wrong".

  ---------------------------------------------------------------------------
  Board: Seeed XIAO ESP32C3   ·   esp32:esp32:XIAO_ESP32C3
  Leave "USB CDC On Boot" ENABLED (its default) or Serial goes to the UART pins
  and this prints into the void.
*/

#include <FastLED.h>

// --- the hardware, as built -----------------------------------------------
// D10 is GPIO10. Not a strapping pin — GPIO2/8/9 (silkscreen D0/D8/D9) are, and
// a signal idling LOW on one of those stops the board booting and looks exactly
// like a dead XIAO. Worth knowing before anyone "just moves the data pin".
#define DATA_PIN     D10
// The WHOLE strip: 144 LED/m over the full ~1 m run. If this number is wrong,
// stage 4 is the stage that says so — see stageEnds().
#define NUM_LEDS     144
#define LED_TYPE     WS2812B
#define COLOR_ORDER  GRB

// 1500, not 1700. The cap governs the LEDs only and the XIAO needs the rest of
// the TalentCell's 2 A ceiling. Measured draw across every SP002E test pattern
// was 1.256 A on 50 pixels — and at 144 the all-amber stage 5 wants roughly
// 2 A at BRIGHTNESS 128, so the cap is no longer a distant guard rail. It is
// now part of what stage 5 tests: the limiter should hold the load AT the cap,
// and a supply that sags anyway is the finding.
#define MAX_MILLIAMPS 1500

// Deliberately not full. Stage 5 is asking whether the supply holds, not how
// bright this thing can get, and a brownout mid-test that resets the board
// reads as a crash rather than as the answer to the question.
#define BRIGHTNESS   128

CRGB leds[NUM_LEDS];

static const uint32_t STAGE_MS = 6000;      // how long each stage holds
static const uint32_t WALK_MS_PER_PIXEL = 90;
// The walk is the one stage whose length is set by the strip rather than by
// patience. Everything else is judged from a still frame.
static const uint32_t WALK_MS = NUM_LEDS * WALK_MS_PER_PIXEL + 1500;
static uint8_t  stage      = 0;
static uint32_t stageStart = 0;
static bool     announced  = false;

void setup() {
  Serial.begin(115200);
  // Wait, but not forever: with no monitor attached this must still run. A
  // sketch that blocks on Serial is a sketch that looks dead when it is fine.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { delay(10); }

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  Serial.println();
  Serial.println(F("=== LED Light Bar — bring-up ==============================="));
  Serial.print  (F("FastLED     : ")); Serial.println(FASTLED_VERSION);
  Serial.print  (F("data pin    : D10 (GPIO"));
  Serial.print(DATA_PIN); Serial.println(F(")"));
  Serial.print  (F("pixels      : ")); Serial.print(NUM_LEDS);
  Serial.print  (F(" (")); Serial.print(NUM_LEDS / 144.0f, 2);
  Serial.println(F(" m at 144/m)"));
  Serial.println(F("colour order: GRB (believed — stage 2 checks it)"));
  Serial.print  (F("power cap   : 5V ")); Serial.print(MAX_MILLIAMPS);
  Serial.println(F("mA"));
  Serial.println(F("If you see nothing at all, read the header of this sketch:"));
  Serial.println(F("the first suspect is 1OE not at GND, not the code."));
  Serial.println(F("============================================================"));

  stageStart = millis();
}

// One pixel, so a fault has nowhere to hide.
void stagePixelZero() {
  FastLED.clear();
  leds[0] = CRGB(40, 0, 0);
  FastLED.show();
}

// Three primaries in turn. Serial says what it should be; your eyes say what it
// is. Any disagreement is the byte order, not the wiring.
void stageColourOrder() {
  static uint8_t which = 255;
  uint8_t now = ((millis() - stageStart) / 1800) % 3;
  FastLED.clear();
  leds[0] = (now == 0) ? CRGB(60, 0, 0)
          : (now == 1) ? CRGB(0, 60, 0)
                       : CRGB(0, 0, 60);
  FastLED.show();
  if (now != which) {
    which = now;
    Serial.print(F("   pixel 0 should now look "));
    Serial.println(now == 0 ? F("RED") : now == 1 ? F("GREEN") : F("BLUE"));
  }
}

// Every pixel addressed once. A gap in the travel names the dead one; a stop
// part-way names where the run gives out.
//
// At 90 ms a pixel the full strip takes NUM_LEDS * 90 ms — 13 s at 144, well
// past the 6 s every other stage holds. So this stage's duration is DERIVED
// (see loop()). With a fixed 6 s it would have advanced at pixel 67 every time,
// which looks exactly like the run giving out half way.
void stageWalk() {
  static int16_t last = -1;
  int16_t i = ((millis() - stageStart) / WALK_MS_PER_PIXEL) % NUM_LEDS;
  FastLED.clear();
  leds[i] = CRGB(0, 50, 30);
  FastLED.show();
  if (i != last) {
    last = i;
    if (i % 10 == 0) { Serial.print(F("   at pixel ")); Serial.println(i); }
  }
}

// Both ends lit and nothing between: NUM_LEDS is right only if the far one is
// the physically last pixel. Off by a few and you would never notice in the fire.
void stageEnds() {
  FastLED.clear();
  leds[0]            = CRGB(50, 25, 0);
  leds[NUM_LEDS - 1] = CRGB(0, 25, 50);
  FastLED.show();
}

// The whole strip at once, amber rather than white — this is the load test the
// SP002E patterns never performed, and it is not the moment to find the 3 A case.
void stageLoad() {
  fill_solid(leds, NUM_LEDS, CRGB(255, 110, 10));
  FastLED.show();
}

void loop() {
  if (!announced) {
    announced = true;
    Serial.print(F("[")); Serial.print(stage + 1); Serial.print(F("/5] "));
    switch (stage) {
      case 0: Serial.println(F("PIXEL ZERO — pixel 0 only, dim red. "
                               "Nothing here means the signal never left the board.")); break;
      case 1: Serial.println(F("COLOUR ORDER — pixel 0 cycles red, green, blue.")); break;
      case 2: Serial.print(F("WALK — one pixel travels 0 -> "));
              Serial.print(NUM_LEDS - 1);
              Serial.println(F(". Watch for a gap, or a stop before the end.")); break;
      case 3: Serial.print(F("THE ENDS — pixel 0 amber, pixel "));
              Serial.print(NUM_LEDS - 1);
              Serial.println(F(" blue, nothing between. "
                               "The blue one should be the last pixel.")); break;
      case 4: Serial.print(F("LOAD — all ")); Serial.print(NUM_LEDS);
              Serial.println(F(" amber. Watch for flicker, a colour shift down "
                               "the run, or the board resetting.")); break;
    }
  }

  switch (stage) {
    case 0: stagePixelZero();   break;
    case 1: stageColourOrder(); break;
    case 2: stageWalk();        break;
    case 3: stageEnds();        break;
    case 4: stageLoad();        break;
  }

  if (millis() - stageStart > (stage == 2 ? WALK_MS : STAGE_MS)) {
    stage = (stage + 1) % 5;
    stageStart = millis();
    announced = false;
    if (stage == 0) Serial.println(F("--- looping ---"));
  }
  FastLED.delay(1000 / 120);
}
