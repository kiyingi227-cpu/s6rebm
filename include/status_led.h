#pragma once
#include <stdint.h>

struct LedState {
  bool    btConnected    = false;
  bool    dualPairOpen   = false;
  bool    auxActive      = false;
  bool    charging       = false;
  bool    lowBattery     = false;
  bool    otaActive      = false;
  uint8_t otaProgressPct = 0;
};

class StatusLed {
public:
  void begin();

  // Direct level helpers (used during early boot / I2C wait / shutdown)
  void setRed(bool on);
  void setWhite(bool on);
  void setRedLevel(uint8_t duty);
  void setWhiteLevel(uint8_t duty);

  void blinkRedCount(int count);
  void triggerButtonAck();

  // Call regularly in loop() (~15-20 ms) for buttery-smooth PWM breathing animations
  void update(const LedState &state);

  // Off-state charging standby: WHITE completely off, RED breathes while charging (off when full)
  void updateStandbyCharging(bool charging);

  [[noreturn]] void haltWithError(int errorCode);

private:
  // Smooth gamma-corrected sine wave in [minDuty .. maxDuty]
  static uint8_t breathDuty(uint32_t nowMs, uint32_t periodMs, uint8_t minDuty, uint8_t maxDuty);

  uint32_t ackUntilMs_ = 0;
};
