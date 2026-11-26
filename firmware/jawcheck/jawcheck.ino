/*
  Jaw check — find the real limits of the servo on the real mechanism.

  The jaw plan (docs/06-jaw-plan.md, phases 1 and 2) needs two numbers before any
  renderer is written: open_angle and closed_angle, set by MEASUREMENT. The S51's
  listing says 0.5-2.5 ms is 180 degrees (docs/07-servo-s51.md), but the end stops
  of a jaw linkage are somewhere inside that, and a servo driven into a stop draws
  stall current until it is told to move.

  So this sketch does one thing: it puts a steady, hardware-timed pulse on D10 and
  lets you move it a little at a time over serial, watching a power meter.

  ---------------------------------------------------------------------------
  COMMANDS (115200, one per line)

    st            pulse width, limits, whether the pulse is running
    us <n>        go to a pulse width in microseconds (clamped to the limits)
    + <n>         step up by n us        - <n>   step down by n us
    ang <deg>     0..180, mapped onto 500..2500 us  (clamped to the limits)
    lim <lo> <hi> set the soft limits. Starts at 1000..2000 us, ~120 degrees
                  and well inside the datasheet range. WIDEN IT ONLY BY WATCHING
                  THE METER, a few tens of us at a time.
    center        1500 us and the default limits
    sweep         slow lo -> hi -> lo, 20 us every 40 ms, then back to rest
    off           stop the pulse (the servo goes limp and draws almost nothing)
    on            resume the pulse at the last width

  ---------------------------------------------------------------------------
  WHY IT IS BUILT THIS WAY

    HARDWARE PWM (LEDC). The S51's dead band is <= 5 us. A pulse timed in software
    can jitter by that much whenever the radio or USB interrupts, and the servo
    will chase it — the jaw buzzes. LEDC counts the pulse in hardware.

    HARD LIMITS. 500..2500 us is the datasheet's range and is never exceeded, no
    matter what is typed. The soft limits sit inside that and start narrow.

    STALL GUARD. A servo against a mechanical stop draws its stall current until
    it is told to move. If the pulse is held outside 1000..2000 us for more than
    3 s it returns to 1500 us on its own. 1000..2000 is not a claim about where
    the stops are; it is the range this sketch trusts a jaw to be inside.

    BOOT. The pulse starts at 1500 us, the middle, not at 0 and not at an
    extreme. Opening the serial port can reset the C3, which re-centres the servo
    — send several commands in ONE ./send.sh call, not one call each.

  Pin: D10 (GPIO10). Never D0, D8 or D9 — see CLAUDE.md, strapping pins.
*/

#include <Arduino.h>

static const int      PIN_SERVO   = D10;
static const uint32_t PWM_HZ      = 50;
static const uint8_t  PWM_BITS    = 14;                 // 16384 counts per 20 ms
static const uint32_t PERIOD_US   = 1000000UL / PWM_HZ; // 20000

static const int US_HARD_MIN = 500,  US_HARD_MAX = 2500;   // the datasheet's range
static const int US_SAFE_LO  = 1000, US_SAFE_HI  = 2000;   // default limits and stall guard
static const int US_CENTER   = 1500;
static const uint32_t STALL_GUARD_MS = 3000;

static int      limLo = US_SAFE_LO, limHi = US_SAFE_HI;
static int      pulseUs = US_CENTER;
static bool     running = false;
static uint32_t outsideSince = 0;                       // 0 = currently inside the safe range

static void writePulse(int us) {
  uint32_t duty = ((uint64_t)us * ((1u << PWM_BITS) - 1)) / PERIOD_US;
  ledcWrite(PIN_SERVO, duty);
}

static int clampUs(int us) {
  if (us < limLo) us = limLo;
  if (us > limHi) us = limHi;
  if (us < US_HARD_MIN) us = US_HARD_MIN;
  if (us > US_HARD_MAX) us = US_HARD_MAX;
  return us;
}

static void goTo(int us) {
  pulseUs = clampUs(us);
  if (!running) { ledcAttach(PIN_SERVO, PWM_HZ, PWM_BITS); running = true; }
  writePulse(pulseUs);
  bool outside = pulseUs < US_SAFE_LO || pulseUs > US_SAFE_HI;
  if (!outside) outsideSince = 0;
  else if (outsideSince == 0) outsideSince = millis();
  Serial.printf("-> %d us (%.1f deg)\n", pulseUs, (pulseUs - US_HARD_MIN) * 180.0f / 2000.0f);
}

static void off() {
  if (running) { ledcDetach(PIN_SERVO); pinMode(PIN_SERVO, OUTPUT); digitalWrite(PIN_SERVO, LOW); running = false; }
  outsideSince = 0;
  Serial.println("-> pulse OFF (servo limp)");
}

static void status() {
  Serial.printf("st pulse=%d us  deg=%.1f  limits=%d..%d  hard=%d..%d  %s\n",
                pulseUs, (pulseUs - US_HARD_MIN) * 180.0f / 2000.0f, limLo, limHi,
                US_HARD_MIN, US_HARD_MAX, running ? "running" : "OFF");
}

static void sweep() {
  Serial.printf("-> sweep %d..%d us\n", limLo, limHi);
  for (int us = pulseUs; us <= limHi; us += 20) { goTo(us); delay(40); }
  for (int us = limHi;   us >= limLo; us -= 20) { goTo(us); delay(40); }
  goTo(US_CENTER);
}

static void handle(String s) {
  s.trim();
  if (!s.length()) return;
  int sp = s.indexOf(' ');
  String cmd = sp < 0 ? s : s.substring(0, sp);
  String arg = sp < 0 ? "" : s.substring(sp + 1);
  arg.trim();

  if      (cmd == "st")     status();
  else if (cmd == "us")     { if (arg.length()) goTo(arg.toInt()); else Serial.println("us <n>"); }
  else if (cmd == "+")      goTo(pulseUs + (arg.length() ? arg.toInt() : 10));
  else if (cmd == "-")      goTo(pulseUs - (arg.length() ? arg.toInt() : 10));
  else if (cmd == "ang")    { int d = constrain(arg.toInt(), 0, 180); goTo(US_HARD_MIN + d * 2000 / 180); }
  else if (cmd == "lim") {
    int sp2 = arg.indexOf(' ');
    if (sp2 < 0) { Serial.println("lim <lo> <hi>"); return; }
    int lo = arg.substring(0, sp2).toInt(), hi = arg.substring(sp2 + 1).toInt();
    if (lo < US_HARD_MIN || hi > US_HARD_MAX || lo >= hi) {
      Serial.printf("-> refused: need %d <= lo < hi <= %d\n", US_HARD_MIN, US_HARD_MAX); return;
    }
    limLo = lo; limHi = hi; Serial.printf("-> limits %d..%d\n", limLo, limHi);
    goTo(pulseUs);                                      // pull the pulse inside the new limits
  }
  else if (cmd == "center") { limLo = US_SAFE_LO; limHi = US_SAFE_HI; goTo(US_CENTER); }
  else if (cmd == "sweep")  sweep();
  else if (cmd == "off")    off();
  else if (cmd == "on")     goTo(pulseUs);
  else Serial.println("? st | us <n> | + <n> | - <n> | ang <deg> | lim <lo> <hi> | center | sweep | off | on");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, LOW);
  Serial.println();
  Serial.println("=== jawcheck — servo on D10, hardware PWM, 50 Hz ===");
  goTo(US_CENTER);
  status();
}

void loop() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handle(line); line = ""; }
    else if (line.length() < 80) line += c;
  }
  if (running && outsideSince && millis() - outsideSince > STALL_GUARD_MS) {
    Serial.println("-> stall guard: outside 1000..2000 us for 3 s, returning to 1500");
    goTo(US_CENTER);
  }
}
