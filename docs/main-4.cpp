/*
 * Power manager v1  (ESP32 DevKit <-> SLG4V4826 standby IC)
 *
 * Findings from the probes:
 *   - After the power button wakes the SLG, VREG_EN goes HIGH.
 *   - The SLG shuts down after ~1 s unless Power_Off_Ready (SLG pin 5) is HIGH.
 *     Handshake accepted as late as 900 ms.  Power_On_Off is NOT needed to stay on.
 *   - Dropping Power_Off_Ready LOW switches the speaker off immediately.
 *
 * Wiring:
 *   D4  -> Power_Off_Ready (SLG pin 5)   output, HIGH = keep power ON
 *   D17 -> Power_On_Off    (SLG pin 4)   output, held LOW (role unknown)
 *   D15 <- VREG_EN         (TP34)                                input   (as wired)
 *   D32 <- BLUE_KEY        (Bluetooth button pad, already wired) input   TEST "off" key
 *   (The real power key is not wired to the ESP32 yet, so for this test the
 *    speaker's Bluetooth button switches it OFF. Power ON uses the real power key.)
 *   D19 -> MUTE            output, HIGH = amps muted
 *
 * Behaviour:
 *   speaker off : wait for VREG_EN HIGH, then assert D4 at once (latch on)
 *   speaker on  : press the Bluetooth (BLUE_KEY) button -> mute, then drop D4 (off)
 *
 * Serial 115200. Do NOT type into the monitor (RX0 = AMP_PWR_CTRL).
 */
#include <Arduino.h>

const int PIN_LATCH     = 4;
const int PIN_ON_OFF    = 17;
const int PIN_POWER_KEY = 32;   // TEST ONLY: BLUE_KEY button pad (already wired), acts as the "off" key
const int PIN_VREG_EN   = 15;   // as already wired
const int PIN_MUTE      = 19;

const uint32_t PRESS_MS = 80;          // a press this long (debounced) turns the speaker off
const uint32_t LOCKOUT_MS = 600;       // ignore the button just after power-on
const uint32_t MUTE_SETTLE_MS = 300;

int keyIdle;                 // level of Power_KEY when not pressed (learned at boot)
bool speakerOn = false;
bool needRelease = false;    // ignore the press that switched the speaker on
uint32_t pressStart = 0;
bool pressing = false;
uint32_t onAt = 0;

inline bool keyPressed() { return digitalRead(PIN_POWER_KEY) != keyIdle; }
inline bool vregActive() { return digitalRead(PIN_VREG_EN) == HIGH; }

void latchOn()  { digitalWrite(PIN_LATCH, HIGH); }
void latchOff() { digitalWrite(PIN_LATCH, LOW); }

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_POWER_KEY, INPUT_PULLUP);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_ON_OFF, OUTPUT);  digitalWrite(PIN_ON_OFF, LOW);
  pinMode(PIN_MUTE, OUTPUT);    digitalWrite(PIN_MUTE, HIGH);   // muted until audio is ready
  pinMode(PIN_LATCH, OUTPUT);

  keyIdle = digitalRead(PIN_POWER_KEY);
  Serial.printf("\n=== Power manager v1 ===\nOff-key (BLUE_KEY) idle level = %d (do not hold it at boot)\n", keyIdle);

  if (vregActive()) {          // ESP32 reset while the speaker is already on
    latchOn();
    speakerOn = true;
    needRelease = true;
    onAt = millis();
    Serial.println("Speaker already ON at boot: latch asserted.");
  } else {
    latchOff();
    Serial.println("Speaker OFF. Press the real power button to start it.");
  }
}

void shutdown() {
  Serial.println("Off-key pressed: shutting down...");
  digitalWrite(PIN_MUTE, HIGH);       // mute amps first to avoid a pop
  delay(MUTE_SETTLE_MS);
  latchOff();                         // SLG cuts the rails
  speakerOn = false;
  needRelease = true;                 // wait for the button to be released
  Serial.println("Latch released. Speaker should be OFF.");
}

void loop() {
  if (!speakerOn) {
    if (vregActive()) {
      uint32_t t0 = millis();
      latchOn();
      speakerOn = true;
      needRelease = keyPressed();     // button may still be held
      onAt = millis();
      Serial.printf("VREG_EN active -> latch asserted (%lu ms). Speaker is ON.\n",
                    (unsigned long)(millis() - t0));
    }
  } else {
    if (!vregActive()) {              // power lost for some other reason
      speakerOn = false;
      latchOff();
      Serial.println("VREG_EN dropped: speaker went OFF by itself.");
    } else {
      if (needRelease) {
        if (!keyPressed()) needRelease = false;
      } else if (millis() - onAt < LOCKOUT_MS) {
        // ignore bounce right after power-on
      } else if (keyPressed()) {
        if (!pressing) { pressing = true; pressStart = millis(); }
        else if (millis() - pressStart >= PRESS_MS) {
          pressing = false;
          shutdown();
        }
      } else if (pressing) {
        pressing = false;
        Serial.println("Glitch ignored (too short).");
      }
    }
  }
  delay(2);
}
