#pragma once
#include <stdint.h>

class StatusLed;

class PowerManager {
public:
  // Returns true on a cold boot where 19 V charger powered SYS_3V3 while the speaker is OFF
  // (VREG_EN == LOW && DC_DET == HIGH). Always returns false on software/OTA reboots (ESP_RST_SW).
  bool isChargerOnlyWake();

  // Off-state charging standby loop (D4 held LOW, amps/DSP/BT off): breathes RED LED while
  // bq24090 /CHG is active (turns OFF when 100% full), returns as soon as Power Button wakes VREG_EN.
  void waitForPowerButtonWithChargingLed(StatusLed &leds);

  // Latch SLG4V4826 power ON immediately (must be called within ~900 ms of wake)
  void beginLatched();

  // Call once startup is finished to start watching D17 (Power_On_Off) for power-off presses
  void armPowerOff();

  // Returns true once a valid debounced Power Button press is detected on D17 (SLG pin 4)
  bool pollPowerOffRequested();

  // Debounced 19 V AC adapter detect on GPIO34 (PIN_DC_DET): true = 19 V AC, false = 10 V battery
  bool pollAcPower();
  bool isAcPower() const { return acPower_; }

  // Drops D4 (Power_Off_Ready) LOW so SLG cuts power.
  // If ESP32 remains powered (19 V charger or USB), shows off-state charging LED until VREG_EN wakes.
  [[noreturn]] void powerOffAndWaitForWake(StatusLed &leds);

private:
  bool     armed_           = false;
  bool     sawIdleLow_      = false;
  uint32_t armedAtMs_       = 0;
  uint32_t highStartMs_     = 0;

  bool     acPower_         = false;
  bool     acPending_       = false;
  uint32_t acChangeStartMs_ = 0;
};
