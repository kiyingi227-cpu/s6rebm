#include "power_manager.h"
#include "board_config.h"
#include "status_led.h"
#include "esp_system.h"

using namespace BoardConfig;

namespace {
constexpr uint32_t POWER_OFF_LOCKOUT_MS  = 1500; // Ignore wake-up press right after startup
constexpr uint32_t POWER_OFF_DEBOUNCE_MS = 60;   // Measured SLG pulse on D17 is ~270 ms on short press
constexpr uint32_t AC_DETECT_DEBOUNCE_MS = 100;  // Debounce 19 V adapter plug/unplug on DC_DET
constexpr uint32_t WAKE_DEBOUNCE_MS      = 20;   // Debounce VREG_EN high when waking from charging standby
} // namespace

bool PowerManager::isChargerOnlyWake() {
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_DC_DET, INPUT);
  pinMode(PIN_CHG_STATUS, INPUT);

  // Never drop into charging standby on software / OTA reboot (must complete boot & confirm OTA!)
  if (esp_reset_reason() == ESP_RST_SW) {
    return false;
  }

  // Only treat as charger-only wake if DC_DET is HIGH and VREG_EN stays LOW for 20 ms
  for (int i = 0; i < 4; i++) {
    if (digitalRead(PIN_DC_DET) == LOW || digitalRead(PIN_VREG_EN) == HIGH) {
      return false;
    }
    delay(5);
  }
  return true;
}

void PowerManager::waitForPowerButtonWithChargingLed(StatusLed &leds) {
  pinMode(PIN_PWR_OFF_READY, OUTPUT);
  digitalWrite(PIN_PWR_OFF_READY, LOW);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_DC_DET, INPUT);
  pinMode(PIN_CHG_STATUS, INPUT);

  uint32_t vregHighSince = 0;
  for (;;) {
    uint32_t now = millis();
    if (digitalRead(PIN_VREG_EN) == HIGH) {
      if (vregHighSince == 0) {
        vregHighSince = now;
      } else if ((now - vregHighSince) >= WAKE_DEBOUNCE_MS) {
        leds.setRed(false);
        leds.setWhite(false);
        return;
      }
    } else {
      vregHighSince = 0;
    }

    // bq24090 /CHG is active-LOW while charging and releases HIGH when 100% full
    bool dcPresent = (digitalRead(PIN_DC_DET) == HIGH);
    bool charging  = dcPresent && (digitalRead(PIN_CHG_STATUS) == LOW);
    leds.updateStandbyCharging(charging);
    delay(15);
  }
}

void PowerManager::beginLatched() {
  pinMode(PIN_PWR_OFF_READY, OUTPUT);
  digitalWrite(PIN_PWR_OFF_READY, HIGH);

  // Pure inputs (no pull-up/down) so SLG4V4826 outputs & dividers drive clean logic levels
  pinMode(PIN_PWR_ON_OFF, INPUT);
  pinMode(PIN_VREG_EN, INPUT);
  pinMode(PIN_PWR_ONOFF_OP, INPUT);
  pinMode(PIN_DC_DET, INPUT);

  acPower_         = (digitalRead(PIN_DC_DET) == HIGH);
  acPending_       = acPower_;
  acChangeStartMs_ = 0;
}

void PowerManager::armPowerOff() {
  armed_       = true;
  sawIdleLow_  = (digitalRead(PIN_PWR_ON_OFF) == LOW);
  armedAtMs_   = millis();
  highStartMs_ = 0;
}

bool PowerManager::pollPowerOffRequested() {
  if (!armed_) return false;

  uint32_t now = millis();
  if (now - armedAtMs_ < POWER_OFF_LOCKOUT_MS) return false;

  int level = digitalRead(PIN_PWR_ON_OFF);
  if (level == LOW) {
    sawIdleLow_  = true;
    highStartMs_ = 0;
    return false;
  }

  // Require D17 to have been LOW at least once before accepting a HIGH pulse
  if (!sawIdleLow_) return false;

  if (highStartMs_ == 0) {
    highStartMs_ = now;
    return false;
  }

  return (now - highStartMs_) >= POWER_OFF_DEBOUNCE_MS;
}

bool PowerManager::pollAcPower() {
  bool raw = (digitalRead(PIN_DC_DET) == HIGH);
  if (raw == acPower_) {
    acPending_       = acPower_;
    acChangeStartMs_ = 0;
    return acPower_;
  }

  uint32_t now = millis();
  if (raw != acPending_ || acChangeStartMs_ == 0) {
    acPending_       = raw;
    acChangeStartMs_ = now;
    return acPower_;
  }

  if ((now - acChangeStartMs_) >= AC_DETECT_DEBOUNCE_MS) {
    acPower_         = raw;
    acChangeStartMs_ = 0;
  }
  return acPower_;
}

void PowerManager::powerOffAndWaitForWake(StatusLed &leds) {
  // Drop SLG pin 5 (Power_Off_Ready) LOW -> SLG immediately cuts Power_ON & VREG_EN
  digitalWrite(PIN_PWR_OFF_READY, LOW);

  // If running on battery, SYS_3V3 cuts immediately right here (0 uA).
  // If 19 V charger (or bench USB) keeps SYS_3V3 powered:
  //   1. Wait for VREG_EN (D15) to go LOW (confirm SLG released speaker rails)
  uint32_t t0 = millis();
  while (digitalRead(PIN_VREG_EN) == HIGH && (millis() - t0) < 2000) {
    delay(10);
  }
  delay(200);

  //   2. Enter Off-State Charging standby (breathe RED while bq24090 charges, OFF when full)
  //      until the next physical Power Button press asserts VREG_EN HIGH.
  waitForPowerButtonWithChargingLed(leds);

  //   3. Restart cleanly WITHOUT pulsing D4 high first (a HIGH->LOW glitch during
  //      reboot would trip SLG shutdown). ESP32 boots into setup() in ~350 ms with
  //      ESP_RST_SW and asserts D4 HIGH cleanly well within the SLG's ~1000 ms wake window.
  ESP.restart();
  for (;;) { delay(1000); }
}
