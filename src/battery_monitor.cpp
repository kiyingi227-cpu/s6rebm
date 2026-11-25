#include "battery_monitor.h"
#include "board_config.h"

using namespace BoardConfig;

void BatteryMonitor::begin() {
  pinMode(PIN_DC_DET, INPUT);
  pinMode(PIN_CHG_STATUS, INPUT);
  pinMode(PIN_BAT_DET, INPUT);
  pinMode(PIN_BAT_TEMP_DET, INPUT);

  // 6 dB attenuation covers ~0.15 V .. 1.75 V cleanly (matches BAT_DET 0.75-1.05 V & NTC 0.4-1.05 V)
  analogSetPinAttenuation(PIN_BAT_DET, ADC_6db);
  analogSetPinAttenuation(PIN_BAT_TEMP_DET, ADC_6db);
}

void BatteryMonitor::poll() {
  uint32_t now = millis();
  if (initialized_ && (now - lastSampleMs_) < 250) return;
  lastSampleMs_ = now;

  // Read eFuse-calibrated millivolts (average 4 readings to reject switching ripple)
  uint32_t batMvSum  = 0;
  uint32_t tempMvSum = 0;
  for (int i = 0; i < 4; i++) {
    batMvSum  += analogReadMilliVolts(PIN_BAT_DET);
    tempMvSum += analogReadMilliVolts(PIN_BAT_TEMP_DET);
  }
  float rawBat  = (static_cast<float>(batMvSum)  * 0.25f / 1000.0f) * BAT_DIVIDER_SCALE;
  float rawTemp = (static_cast<float>(tempMvSum) * 0.25f / 1000.0f);

  if (!initialized_) {
    vBat_        = rawBat;
    vTemp_       = rawTemp;
    initialized_ = true;
  } else {
    vBat_  = 0.85f * vBat_  + 0.15f * rawBat;
    vTemp_ = 0.85f * vTemp_ + 0.15f * rawTemp;
  }

  dcPresent_ = (digitalRead(PIN_DC_DET) == HIGH);
  // bq24090 /CHG is open-drain active-LOW (pulled up to SYS_3V3 by R50 10k)
  charging_  = dcPresent_ && (digitalRead(PIN_CHG_STATUS) == LOW);

  bool batteryPresent = (vBat_ >= BAT_PRESENT_MIN_V);

  // Thermal guard (NTC voltage drops as temperature rises; ignore 0 V unwired bench state)
  overTemp_ = (vTemp_ >= BAT_TEMP_MIN_V && vTemp_ <= BAT_TEMP_HOT_V);

  // Low-battery warning with hysteresis (only when running on battery)
  if (!batteryPresent || dcPresent_) {
    lowBattery_ = false;
  } else {
    if (vBat_ <= BAT_LOW_V)          lowBattery_ = true;
    else if (vBat_ >= BAT_RECOVER_V) lowBattery_ = false;
  }

  // Sustained critical low-voltage or over-temperature shutdown
  bool criticalNow = overTemp_ || (batteryPresent && !dcPresent_ && vBat_ <= BAT_CRITICAL_V);
  if (criticalNow) {
    if (critStartMs_ == 0) {
      critStartMs_ = now;
    } else if ((now - critStartMs_) >= BAT_CUTOFF_HOLD_MS) {
      emergencyOff_ = true;
    }
  } else {
    critStartMs_ = 0;
  }
}
