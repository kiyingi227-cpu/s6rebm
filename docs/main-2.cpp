/*
 * Power handshake probe  (ESP32 DevKit -> SLG4V4826 standby IC)
 *
 * Goal: find out what the SLG4V4826 expects from the (removed) BM20IK
 * so the speaker stays on after the power button is pressed.
 *
 * Wires:
 *   D17 -> Power_On_Off     (SLG4V4826 pin 4)   already wired
 *   D4  -> Power_Off_Ready  (SLG4V4826 pin 5)   already wired
 *   D15 <- VREG_EN          (SLG pin 11 side, test point TP34)  NEW wire, input only
 *   GND common with the speaker, ESP32 powered from USB.
 *
 * Each time you press the power button the probe tries a different
 * handshake and reports whether the speaker stayed on.
 * Serial monitor 115200. Do NOT type into the monitor (RX0 = AMP_PWR_CTRL).
 */
#include <Arduino.h>

const int PIN_ON_OFF   = 17;
const int PIN_OFF_RDY  = 4;
const int PIN_VREG_EN  = 15;

struct Ev { uint16_t t; uint8_t pin; uint8_t lvl; };   // pin: 0 = ON_OFF, 1 = OFF_RDY
struct Strat { const char* name; uint8_t idle0, idle1; uint8_t n; Ev ev[3]; };

const Strat S[] = {
  {"baseline: no handshake",                 0, 0, 0, {}},
  {"ON_OFF high at 100 ms",                  0, 0, 1, {{100,0,1}}},
  {"ON_OFF + OFF_RDY high at 100 ms",        0, 0, 2, {{100,0,1},{100,1,1}}},
  {"OFF_RDY high at 100 ms",                 0, 0, 1, {{100,1,1}}},
  {"ON_OFF pulse high 100 ms",               0, 0, 2, {{100,0,1},{200,0,0}}},
  {"ON_OFF idle high, goes low at 100 ms",   1, 0, 1, {{100,0,0}}},
  {"both idle high, ON_OFF low at 100 ms",   1, 1, 1, {{100,0,0}}},
  {"both idle high, both low at 100 ms",     1, 1, 2, {{100,0,0},{100,1,0}}},
};
const int N = sizeof(S) / sizeof(S[0]);

int idleVreg;
int attempt = 0;

int pinOf(uint8_t p) { return p == 0 ? PIN_ON_OFF : PIN_OFF_RDY; }

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_ON_OFF, OUTPUT);
  pinMode(PIN_OFF_RDY, OUTPUT);
  digitalWrite(PIN_ON_OFF, LOW);
  digitalWrite(PIN_OFF_RDY, LOW);
  delay(200);
  idleVreg = digitalRead(PIN_VREG_EN);
  Serial.printf("\n=== Power probe ===\nVREG_EN idle level = %d (speaker should be OFF now)\n", idleVreg);
}

void loop() {
  const Strat &s = S[attempt % N];
  digitalWrite(PIN_ON_OFF, s.idle0);
  digitalWrite(PIN_OFF_RDY, s.idle1);
  Serial.printf("\n[Attempt %d] %s\nPress the power button now...\n", attempt + 1, s.name);

  while (digitalRead(PIN_VREG_EN) == idleVreg) delay(1);
  uint32_t t0 = millis();
  Serial.printf("  VREG_EN went active (%d)\n", !idleVreg);

  uint8_t next = 0;
  bool latched = false;
  while (true) {
    uint32_t dt = millis() - t0;
    while (next < s.n && dt >= s.ev[next].t) {
      digitalWrite(pinOf(s.ev[next].pin), s.ev[next].lvl);
      Serial.printf("  +%lu ms: %s -> %d\n", (unsigned long)dt,
                    s.ev[next].pin == 0 ? "ON_OFF" : "OFF_RDY", s.ev[next].lvl);
      next++;
    }
    if (digitalRead(PIN_VREG_EN) == idleVreg) {
      Serial.printf("  RESULT: speaker shut down after %lu ms\n", (unsigned long)dt);
      break;
    }
    if (dt > 8000) {
      Serial.println("  RESULT: STAYED ON > 8 s  ==> THIS HANDSHAKE WORKS");
      latched = true;
      break;
    }
    delay(1);
  }

  if (latched) {
    Serial.println("  Press the power button again to turn it off...");
    while (digitalRead(PIN_VREG_EN) != idleVreg) delay(10);
    Serial.printf("  Speaker turned off %lu ms after start\n", (unsigned long)(millis() - t0));
  } else {
    while (digitalRead(PIN_VREG_EN) != idleVreg) delay(10);
  }
  delay(1500);
  attempt++;
}
