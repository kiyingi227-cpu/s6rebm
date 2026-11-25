#pragma once
#include <stdint.h>

class BatteryMonitor {
public:
  void begin();
  void poll();

  float batteryVoltage() const { return vBat_; }
  float tempVoltage()    const { return vTemp_; }
  bool  isDcPresent()    const { return dcPresent_; }
  bool  isCharging()     const { return charging_; }
  bool  isLowBattery()   const { return lowBattery_; }
  bool  isOverTemp()     const { return overTemp_; }
  bool  shouldEmergencyShutdown() const { return emergencyOff_; }

private:
  float    vBat_            = 3.80f;
  float    vTemp_           = 0.80f;
  bool     initialized_     = false;
  bool     dcPresent_       = false;
  bool     charging_        = false;
  bool     lowBattery_      = false;
  bool     overTemp_        = false;
  bool     emergencyOff_    = false;
  uint32_t lastSampleMs_    = 0;
  uint32_t critStartMs_     = 0;
};
