/*
  Power-OFF Signal Probe (SLG4V4826 -> ESP32)

  Goal: Observe SLG pin 4 (Power_On_Off -> D17) and SLG pin 11 (VREG_EN -> D15)
  as pure INPUTs while the speaker is latched ON (D4 = HIGH) and you press / hold
  the real Power Button.

  Safety:
    - MUTE (D19) held MUTED (LOW).
    - DSP_PDN (D5) held LOW.
    - Serial monitor @ 115200 baud. Do NOT type into the monitor (RX0 = AMP_PWR_CTRL).

  Bonus:
    - Also logs D32/D33/D13/D18 (front buttons) so we learn their polarity for free.
    - Hold BLUE_KEY (D32) for >1 s if you want the probe to drop D4 LOW (power off).
*/
#include <Arduino.h>
#include "board_config.h"

using namespace BoardConfig;

static int lastOnOff  = -1;
static int lastVreg   = -1;
static int lastBlue   = -1;
static int lastVolUp  = -1;
static int lastVolDn  = -1;
static int lastPlay   = -1;

static uint32_t onOffChangedAt = 0;
static uint32_t vregChangedAt  = 0;
static bool     latched        = false;
static uint32_t lastBeatMs     = 0;
static int      blueIdle       = 1;
static uint32_t blueHoldStart  = 0;

void setup() {
  // Keep amps muted & DSP powered down
  pinMode(PIN_MUTE, OUTPUT);
  digitalWrite(PIN_MUTE, MUTE_ON);
  pinMode(PIN_DSP_PDN, OUTPUT);
  digitalWrite(PIN_DSP_PDN, !PDN_RUN);

  // Pure INPUTs (no pull-up/down) so we read true SLG output levels without loading
  pinMode(PIN_PWR_ON_OFF, INPUT);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_DC_DET, INPUT);

  // Front buttons (read-only)
  pinMode(PIN_BTN_BLUE, INPUT_PULLUP);
  pinMode(PIN_BTN_VOL_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_VOL_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY_PAUSE, INPUT_PULLUP);

  pinMode(PIN_PWR_OFF_READY, OUTPUT);

  Serial.begin(115200);
  delay(400);

  blueIdle = digitalRead(PIN_BTN_BLUE);

  int vreg = digitalRead(PIN_VREG_EN);
  if (vreg == HIGH) {
    digitalWrite(PIN_PWR_OFF_READY, HIGH);
    latched = true;
  } else {
    digitalWrite(PIN_PWR_OFF_READY, LOW);
    latched = false;
  }

  lastOnOff = digitalRead(PIN_PWR_ON_OFF);
  lastVreg  = digitalRead(PIN_VREG_EN);
  lastBlue  = digitalRead(PIN_BTN_BLUE);
  lastVolUp = digitalRead(PIN_BTN_VOL_UP);
  lastVolDn = digitalRead(PIN_BTN_VOL_DOWN);
  lastPlay  = digitalRead(PIN_BTN_PLAY_PAUSE);
  onOffChangedAt = vregChangedAt = millis();

  Serial.println("\n====================================================");
  Serial.println("  Onyx Studio 5 — Power_On_Off (D17) / VREG_EN Probe");
  Serial.println("  Do NOT type into serial monitor (RX0=AMP_PWR_CTRL)");
  Serial.println("====================================================");
  Serial.printf("Boot state: latched=%d  D4(OFF_RDY)=%d  D17(ON_OFF)=%d  D15(VREG_EN)=%d  D34(DC_DET)=%d\n",
                latched, digitalRead(PIN_PWR_OFF_READY), lastOnOff, lastVreg, digitalRead(PIN_DC_DET));
  Serial.printf("Buttons idle: BLUE(32)=%d  VOL+(33)=%d  VOL-(13)=%d  PLAY(18)=%d\n",
                lastBlue, lastVolUp, lastVolDn, lastPlay);

  if (!latched) {
    Serial.println("\nSpeaker is OFF. Press the POWER button to wake & latch it...");
  } else {
    Serial.println("\nSpeaker is LATCHED ON. Now try:");
    Serial.println("  1. Short-press the POWER button");
    Serial.println("  2. Long-press (2-3 s) the POWER button");
    Serial.println("  3. (Optional) Press VOL+, VOL-, PLAY, BLUE buttons");
  }
}

void loop() {
  uint32_t now = millis();

  int vreg  = digitalRead(PIN_VREG_EN);
  int onOff = digitalRead(PIN_PWR_ON_OFF);

  // Auto-latch on rising VREG_EN
  if (!latched && vreg == HIGH) {
    digitalWrite(PIN_PWR_OFF_READY, HIGH);
    latched = true;
    Serial.printf("[%8lu ms] >>> VREG_EN HIGH -> LATCHED ON (D4=1), D17(ON_OFF)=%d <<<\n",
                  (unsigned long)now, onOff);
    Serial.println("Now try short-press and long-press (2-3 s) on the POWER button!");
  } else if (latched && vreg == LOW) {
    digitalWrite(PIN_PWR_OFF_READY, LOW);
    latched = false;
    Serial.printf("[%8lu ms] >>> VREG_EN dropped LOW -> speaker went OFF, D17(ON_OFF)=%d <<<\n",
                  (unsigned long)now, onOff);
  }

  if (onOff != lastOnOff) {
    Serial.printf("[%8lu ms] D17 Power_On_Off : %d -> %d  (held %lu ms)\n",
                  (unsigned long)now, lastOnOff, onOff, (unsigned long)(now - onOffChangedAt));
    lastOnOff = onOff;
    onOffChangedAt = now;
  }

  if (vreg != lastVreg) {
    Serial.printf("[%8lu ms] D15 VREG_EN      : %d -> %d  (held %lu ms)\n",
                  (unsigned long)now, lastVreg, vreg, (unsigned long)(now - vregChangedAt));
    lastVreg = vreg;
    vregChangedAt = now;
  }

  // Log front button changes (if pressed)
  int bBlue = digitalRead(PIN_BTN_BLUE);
  int bUp   = digitalRead(PIN_BTN_VOL_UP);
  int bDn   = digitalRead(PIN_BTN_VOL_DOWN);
  int bPlay = digitalRead(PIN_BTN_PLAY_PAUSE);

  if (bBlue != lastBlue) {
    Serial.printf("[%8lu ms] BTN BLUE (D32)   : %d -> %d\n", (unsigned long)now, lastBlue, bBlue);
    lastBlue = bBlue;
    blueHoldStart = (bBlue != blueIdle) ? now : 0;
  }
  if (bUp != lastVolUp) {
    Serial.printf("[%8lu ms] BTN VOL+ (D33)   : %d -> %d\n", (unsigned long)now, lastVolUp, bUp);
    lastVolUp = bUp;
  }
  if (bDn != lastVolDn) {
    Serial.printf("[%8lu ms] BTN VOL- (D13)   : %d -> %d\n", (unsigned long)now, lastVolDn, bDn);
    lastVolDn = bDn;
  }
  if (bPlay != lastPlay) {
    Serial.printf("[%8lu ms] BTN PLAY (D18)   : %d -> %d\n", (unsigned long)now, lastPlay, bPlay);
    lastPlay = bPlay;
  }

  // Hold BLUE_KEY > 1.5 s to manually drop latch (D4=0) for re-testing power-on
  if (latched && blueHoldStart != 0 && (now - blueHoldStart) > 1500) {
    Serial.printf("[%8lu ms] BLUE held >1.5s -> dropping D4 LOW (power off)...\n", (unsigned long)now);
    digitalWrite(PIN_PWR_OFF_READY, LOW);
    latched = false;
    blueHoldStart = 0;
    delay(500);
  }

  // Periodic heartbeat every 5 s
  if (now - lastBeatMs >= 5000) {
    lastBeatMs = now;
    Serial.printf("[%8lu ms] [heartbeat] latched=%d  D17(ON_OFF)=%d  D15(VREG_EN)=%d  D34(DC_DET)=%d\n",
                  (unsigned long)now, latched, onOff, vreg, digitalRead(PIN_DC_DET));
  }

  delay(1);
}
