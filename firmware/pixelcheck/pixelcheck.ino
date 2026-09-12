/*
  Pixel check — is the fault one pixel, one channel, or the whole frame?

  Written after `bringup` found two things on the first strip test:

    * pixel 0 shows red and blue but not green
    * the walking dot was hard to follow, so "all 144 addressable" is unconfirmed

  Under GRB, **green is the first byte of the first pixel** — the most exposed
  byte in the entire frame, and the one a marginal first edge eats. So "pixel 0
  has no green" has two candidate causes that predict exactly the same symptom:

    A  pixel 0's green die is dead
    B  the frame's first byte is being corrupted on the wire

  Nothing you can see distinguishes A from B, because byte 1 drives the green
  die either way. What DOES distinguish them is whether the rest of the strip is
  healthy, so this sketch asks that first and holds every state long enough to
  study rather than catch.

  All motion is removed. A travelling dot is a bad instrument: it asks you to
  judge position and time at once, and 90 ms per pixel is faster than that is
  fair. Stage 4 here lights fixed markers and leaves them on.

  ---------------------------------------------------------------------------
  STAGES — each held 4 s, announced on serial

    1  WHOLE STRIP RED      every pixel, red
    2  WHOLE STRIP GREEN    <-- THE ONE THAT MATTERS
    3  WHOLE STRIP BLUE
    4  MARKERS              six pixels spread across the strip, white, held
    5  PIXEL 0 ALONE        red, then green, then blue
    6  PIXEL 1 ALONE        red, then green, then blue
    7  SLOW WALK            400 ms per pixel, slow enough to follow

  ---------------------------------------------------------------------------
  HOW TO READ STAGE 2 — whole strip green

    Green everywhere EXCEPT pixel 0
      -> the fault is confined to the first pixel. Now compare stages 5 and 6:
         if pixel 1 does green and pixel 0 does not, it is that one LED or that
         one byte. Everything downstream is fine, which also means the data is
         being clocked through pixel 0 correctly — so its logic is alive even if
         its green die or its first byte is not.

    No green ANYWHERE
      -> not a pixel fault at all. Either the colour order is wrong, or the
         green data is not surviving the wire. Say so and I will reflash with
         RGB order to see what moves.

    Green everywhere INCLUDING pixel 0
      -> stage 2 of `bringup` was 1.8 s of a dim single pixel and easy to miss.
         Good outcome; we move on.

  HOW TO READ STAGE 4 — markers

    Six lit pixels, held steady, at roughly 0 / 10 / 20 / 30 / 40 / and the very
    end. Count them and check the last one is physically the last pixel. If you
    see fewer than six, the number you DO see says where the run stops. This is
    the same question stage 3 of `bringup` asked, without asking you to track a
    moving object to answer it.
*/

#include <FastLED.h>

#define DATA_PIN     D10
// The WHOLE strip: 144 LED/m over the full ~1 m run.
#define NUM_LEDS     144
#define LED_TYPE     WS2812B
#define COLOR_ORDER  GRB
#define MAX_MILLIAMPS 1500

// Bright enough to judge colour, not so bright that a whole strip of one
// primary sits on the power cap and gets scaled — a limiter quietly dimming
// the strip would look like the very sag we are trying to rule out.
#define BRIGHTNESS   90

CRGB leds[NUM_LEDS];

// Six markers SPREAD over whatever the strip is, rather than every tenth pixel.
// Fixed tenths were six evenly spaced markers on 50 pixels and would be five
// bunched in the first third of 144 with one far away at the end — which asks
// you to judge a gap instead of counting to six.
static const uint8_t  N_MARKERS = 6;
static uint16_t MARKERS[N_MARKERS];
static void buildMarkers() {
  for (uint8_t m = 0; m < N_MARKERS; m++)
    MARKERS[m] = (uint16_t)((uint32_t)m * (NUM_LEDS - 1) / (N_MARKERS - 1));
}

static const uint32_t HOLD_MS = 4000;
static uint8_t  stage = 0;
static uint32_t stageStart = 0;
static bool     announced = false;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { delay(10); }

  buildMarkers();
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  Serial.println();
  Serial.println(F("=== Pixel check — where exactly is the fault? ============="));
  Serial.println(F("Nothing moves in this test. Every state is held 4s."));
  Serial.print  (F("pixels: ")); Serial.println(NUM_LEDS);
  Serial.println(F("The question that matters is stage 2: whole strip green."));
  Serial.println(F("  green everywhere but pixel 0 -> the fault is pixel 0 alone"));
  Serial.println(F("  no green anywhere           -> colour order or the wire"));
  Serial.println(F("=========================================================="));
  stageStart = millis();
}

// One primary across the whole strip, held. The cheapest way to ask "is this
// channel alive anywhere" — and a dead channel on ONE pixel is instantly
// obvious as a gap in an otherwise solid bar.
void solid(const CRGB& c) { fill_solid(leds, NUM_LEDS, c); FastLED.show(); }

void markers() {
  FastLED.clear();
  for (uint8_t k = 0; k < N_MARKERS; k++) leds[MARKERS[k]] = CRGB(70, 70, 70);
  FastLED.show();
}

// One pixel, one primary at a time, 1.3s each. Slower than bringup's 1.8s felt,
// because here you know what you are looking for.
void onePixel(uint16_t idx) {
  static uint8_t which = 255;
  uint8_t now = ((millis() - stageStart) / 1300) % 3;
  FastLED.clear();
  leds[idx] = (now == 0) ? CRGB(120, 0, 0)
            : (now == 1) ? CRGB(0, 120, 0)
                         : CRGB(0, 0, 120);
  FastLED.show();
  if (now != which) {
    which = now;
    Serial.print(F("      pixel ")); Serial.print(idx);
    Serial.print(F(" should be "));
    Serial.println(now == 0 ? F("RED") : now == 1 ? F("GREEN") : F("BLUE"));
  }
}

void slowWalk() {
  static int16_t last = -1;
  int16_t i = ((millis() - stageStart) / 400) % NUM_LEDS;
  FastLED.clear();
  leds[i] = CRGB(80, 80, 0);
  FastLED.show();
  if (i != last) {
    last = i;
    if (i % 5 == 0) { Serial.print(F("      at pixel ")); Serial.println(i); }
  }
}

void loop() {
  uint32_t hold = (stage == 4 || stage == 5) ? 4200
                // Derived, not a constant: at 400 ms a pixel the walk takes
                // NUM_LEDS * 400 ms, which is 21 s at 50 pixels and 58 s at
                // 144. A fixed 21 s would stop the dot a third of the way
                // along and look identical to the run giving out there.
                : (stage == 6)               ? (uint32_t)NUM_LEDS * 400 + 1000
                                             : HOLD_MS;

  if (!announced) {
    announced = true;
    Serial.print(F("[")); Serial.print(stage + 1); Serial.print(F("/7] "));
    switch (stage) {
      case 0: Serial.println(F("WHOLE STRIP RED")); break;
      case 1: Serial.println(F("WHOLE STRIP GREEN  <-- look at pixel 0 "
                               "against the rest")); break;
      case 2: Serial.println(F("WHOLE STRIP BLUE")); break;
      case 3: Serial.print(F("MARKERS — white and held at "));
              for (uint8_t m = 0; m < N_MARKERS; m++) {
                Serial.print(MARKERS[m]);
                if (m + 1 < N_MARKERS) Serial.print(F(", "));
              }
              Serial.println(F(". Count six, and check the last is the last.")); break;
      case 4: Serial.println(F("PIXEL 0 ALONE")); break;
      case 5: Serial.println(F("PIXEL 1 ALONE — the control for pixel 0")); break;
      case 6: Serial.println(F("SLOW WALK — 400ms per pixel, easy to follow")); break;
    }
  }

  switch (stage) {
    case 0: solid(CRGB(120, 0, 0));   break;
    case 1: solid(CRGB(0, 120, 0));   break;
    case 2: solid(CRGB(0, 0, 120));   break;
    case 3: markers();                break;
    case 4: onePixel(0);              break;
    case 5: onePixel(1);              break;
    case 6: slowWalk();               break;
  }

  if (millis() - stageStart > hold) {
    stage = (stage + 1) % 7;
    stageStart = millis();
    announced = false;
    if (stage == 0) Serial.println(F("--- looping ---"));
  }
  FastLED.delay(1000 / 120);
}
