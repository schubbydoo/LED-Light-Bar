/*
  Meter check — prove the signal chain before a single pixel is at risk.

  Run this with the strip DISCONNECTED. It holds D10 at steady DC levels long
  enough to read with an ordinary multimeter, and says on serial what each
  probe point should read. If every measurement below is right, the only thing
  left untested between the XIAO and the strip is the JST and the strip itself.

  Why steady DC rather than a blink: a multimeter cannot follow a 400 ns WS2812
  bit, and averaging a data stream gives a meaningless number somewhere between
  the rails. Three seconds of solid HIGH is a number you can trust.

  ---------------------------------------------------------------------------
  WHAT TO MEASURE, black probe on the GND bus throughout

  Rails first, before you look at anything else:

      +5 V bus            5.0 - 5.2 V     TalentCell, on
      XIAO 5V pad         4.55 - 4.7 V    the Schottky's drop; NOT 5.0
      U1 pin 14 (VCC)     same as the bus
      U1 pin 7  (GND)     0 V
      U1 pin 1  (1OE)     0 V             must be a hard 0, not "near 0"

  Then the two stages this sketch alternates:

      stage        U1 pin 2 (1A, in)   U1 pin 3 (1Y, out)
      ---------    -----------------   ------------------
      D10 LOW            0 V                  0 V
      D10 HIGH          3.3 V               ~5.0 V     <-- the whole point

  **Pin 3 reading ~5 V while pin 2 reads 3.3 V is the level shifter working.**
  That one measurement proves the XIAO reaches the buffer, that 1OE is really
  grounded, that VCC is really 5 V, and that the part is genuinely an HCT and
  not an HC. Everything this circuit exists to do is in that line.

  ---------------------------------------------------------------------------
  IF PIN 3 IS WRONG

    Pin 3 floats, or reads some in-between voltage that drifts as you touch it
      -> the output is high-impedance. Pin 1 (1OE) is not actually at GND.
         Meter pin 1 to the GND bus in continuity mode; do not trust that it
         looks wired. This is the top suspect and it always has been.

    Pin 3 follows pin 2 at 3.3 V instead of 5 V
      -> VCC is not 5 V. Meter pin 14. A buffer running from 3.3 V buys nothing.

    Pin 3 stays at 0 V while pin 2 swings correctly
      -> either 1OE again, or the chip is in backwards, or it is dead. A
         reversed socket kills it instantly and the docs warn about it.

    Pin 2 never leaves 0 V
      -> the D10 wire. Note the XIAO's silkscreen is on its UNDERSIDE, so it is
         easy to count pads from the wrong edge.

  ---------------------------------------------------------------------------
  D10 IS PARKED AS AN INPUT FOR THE FIRST 10 SECONDS

  Driving a pin into an unpowered chip pushes current through its input clamp
  diode into a dead VCC net. TI rates that at 20 mA and an unloaded GPIO can
  exceed it. So this sketch comes up with D10 high-impedance and tells you to
  switch the TalentCell on first. Not a big risk, but a free one to avoid, and
  it is the state to leave the board in whenever the bus is off.
*/

#define DATA_PIN D10

static const uint32_t ARM_MS   = 10000;   // hi-Z grace period at boot
static const uint32_t STAGE_MS = 6000;    // long enough to settle a meter

static bool     armed = false;
static uint8_t  stage = 0;
static uint32_t stageStart = 0;
static bool     announced = false;

void setup() {
  // Input FIRST, before anything else can drive it.
  pinMode(DATA_PIN, INPUT);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { delay(10); }

  Serial.println();
  Serial.println(F("=== Meter check — signal chain, no pixels ================="));
  Serial.println(F("STRIP MUST BE DISCONNECTED."));
  Serial.println(F("D10 is high-impedance for 10s. Switch the TalentCell ON now"));
  Serial.println(F("if it is not already — driving D10 into an unpowered buffer"));
  Serial.println(F("pushes current through its input clamp diode."));
  Serial.println();
  Serial.println(F("Black probe on the GND bus. Check the rails while you wait:"));
  Serial.println(F("   +5V bus         5.0 - 5.2 V"));
  Serial.println(F("   XIAO 5V pad     4.55 - 4.7 V   (the Schottky drop, not 5.0)"));
  Serial.println(F("   U1 pin 14       same as the bus"));
  Serial.println(F("   U1 pin 7        0 V"));
  Serial.println(F("   U1 pin 1        0 V  — a hard zero, this is 1OE"));
  Serial.println(F("=========================================================="));

  stageStart = millis();
}

void loop() {
  if (!armed) {
    if (millis() - stageStart < ARM_MS) { delay(50); return; }
    armed = true;
    pinMode(DATA_PIN, OUTPUT);
    stageStart = millis();
    announced = false;
    Serial.println();
    Serial.println(F("--- D10 is now driving. Probe U1 pin 2 and pin 3. ---"));
  }

  if (!announced) {
    announced = true;
    if (stage == 0) {
      digitalWrite(DATA_PIN, LOW);
      Serial.println(F("[LOW ] D10 held LOW    -> pin 2 = 0 V      pin 3 = 0 V"));
    } else {
      digitalWrite(DATA_PIN, HIGH);
      Serial.println(F("[HIGH] D10 held HIGH   -> pin 2 = 3.3 V    pin 3 = ~5.0 V"));
      Serial.println(F("       ^ pin 3 at 5 V while pin 2 is 3.3 V is the whole job."));
      Serial.println(F("         Anything else: read this sketch's header."));
    }
  }

  if (millis() - stageStart > STAGE_MS) {
    stage ^= 1;
    stageStart = millis();
    announced = false;
  }
  delay(20);
}
