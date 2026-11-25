/*
 * Power probe 2: find out which line turns the speaker OFF, and how long
 * the SLG4V4826 waits for the handshake.
 *
 * Wires (same as probe 1):
 *   D17 -> Power_On_Off (SLG pin 4)   D4 -> Power_Off_Ready (SLG pin 5)
 *   D15 <- VREG_EN (input)
 *
 * Press the power button ONCE per trial when asked. Each trial:
 *   1. does a handshake after VREG_EN goes active
 *   2. if the speaker stays on for 5 s, it RELEASES the chosen line(s)
 *   3. reports whether the speaker switched off
 * Serial 115200. Do NOT type into the monitor (RX0 = AMP_PWR_CTRL).
 */
#include <Arduino.h>

const int PIN_ON_OFF  = 17;
const int PIN_OFF_RDY = 4;
const int PIN_VREG_EN = 15;

struct Ev { uint16_t t; uint8_t pin; uint8_t lvl; };      // pin 0 = ON_OFF, 1 = OFF_RDY
struct Trial {
  const char* name;
  uint8_t n; Ev ev[2];
  uint8_t releaseMask;                                    // bit0 = ON_OFF low, bit1 = OFF_RDY low
};

const Trial T[] = {
  {"OFF_RDY alone high @100ms; release OFF_RDY",   1, {{100,1,1}},            2},
  {"both high @100ms; release ON_OFF only",        2, {{100,0,1},{100,1,1}},  1},
  {"both high @100ms; release OFF_RDY only",       2, {{100,0,1},{100,1,1}},  2},
  {"both high @100ms; release both",               2, {{100,0,1},{100,1,1}},  3},
  {"both high @300ms (timing)",                    2, {{300,0,1},{300,1,1}},  3},
  {"both high @500ms (timing)",                    2, {{500,0,1},{500,1,1}},  3},
  {"both high @700ms (timing)",                    2, {{700,0,1},{700,1,1}},  3},
  {"both high @900ms (timing)",                    2, {{900,0,1},{900,1,1}},  3},
};
const int N = sizeof(T) / sizeof(T[0]);
int idleV, trial = 0;

int pinOf(uint8_t p) { return p ? PIN_OFF_RDY : PIN_ON_OFF; }

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_ON_OFF, OUTPUT);  digitalWrite(PIN_ON_OFF, LOW);
  pinMode(PIN_OFF_RDY, OUTPUT); digitalWrite(PIN_OFF_RDY, LOW);
  delay(200);
  idleV = digitalRead(PIN_VREG_EN);
  Serial.printf("\n=== Power probe 2 ===  VREG_EN idle = %d\n", idleV);
}

void waitOff(const char* msg) {
  if (digitalRead(PIN_VREG_EN) != idleV) Serial.println(msg);
  while (digitalRead(PIN_VREG_EN) != idleV) delay(10);
}

void loop() {
  const Trial &t = T[trial % N];
  digitalWrite(PIN_ON_OFF, LOW);
  digitalWrite(PIN_OFF_RDY, LOW);
  Serial.printf("\n[Trial %d/%d] %s\nPress the power button once...\n", trial % N + 1, N, t.name);

  while (digitalRead(PIN_VREG_EN) == idleV) delay(1);
  uint32_t t0 = millis();
  uint8_t next = 0;
  bool alive = true;

  // phase 1: handshake, then watch for 5 s
  while (millis() - t0 < 5000) {
    uint32_t dt = millis() - t0;
    while (next < t.n && dt >= t.ev[next].t) {
      digitalWrite(pinOf(t.ev[next].pin), t.ev[next].lvl);
      Serial.printf("  +%lu ms: %s -> %d\n", (unsigned long)dt,
                    t.ev[next].pin ? "OFF_RDY" : "ON_OFF", t.ev[next].lvl);
      next++;
    }
    if (digitalRead(PIN_VREG_EN) == idleV) {
      Serial.printf("  speaker shut down after %lu ms (handshake too late or wrong)\n",
                    (unsigned long)(millis() - t0));
      alive = false;
      break;
    }
    delay(1);
  }

  // phase 2: speaker stayed on -> release the lines
  if (alive) {
    Serial.println("  stayed on for 5 s = handshake OK");
    if (t.releaseMask & 1) { digitalWrite(PIN_ON_OFF, LOW);  Serial.println("  releasing ON_OFF -> 0"); }
    if (t.releaseMask & 2) { digitalWrite(PIN_OFF_RDY, LOW); Serial.println("  releasing OFF_RDY -> 0"); }
    uint32_t t1 = millis();
    bool off = false;
    while (millis() - t1 < 4000) {
      if (digitalRead(PIN_VREG_EN) == idleV) { off = true; break; }
      delay(1);
    }
    if (off) Serial.printf("  RESULT: speaker switched OFF %lu ms after release\n", (unsigned long)(millis() - t1));
    else     Serial.println("  RESULT: still ON after release (this release does NOT turn it off)");
    digitalWrite(PIN_ON_OFF, LOW);
    digitalWrite(PIN_OFF_RDY, LOW);
  }

  waitOff("  waiting for speaker to turn off (hold the power button if it stays on)...");
  delay(2000);
  trial++;
}
