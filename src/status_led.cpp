#include "status_led.h"
#include "board_config.h"
#include <math.h>

using namespace BoardConfig;

void StatusLed::begin() {
  ledcSetup(LED_CH_WHITE, LED_PWM_FREQ, LED_PWM_BITS);
  ledcAttachPin(PIN_WHITE_LED, LED_CH_WHITE);

  ledcSetup(LED_CH_RED, LED_PWM_FREQ, LED_PWM_BITS);
  ledcAttachPin(PIN_RED_LED, LED_CH_RED);

  setWhiteLevel(0);
  setRedLevel(0);
}

void StatusLed::setRedLevel(uint8_t duty) {
  ledcWrite(LED_CH_RED, (LED_ON == HIGH) ? duty : static_cast<uint8_t>(255 - duty));
}

void StatusLed::setWhiteLevel(uint8_t duty) {
  ledcWrite(LED_CH_WHITE, (LED_ON == HIGH) ? duty : static_cast<uint8_t>(255 - duty));
}

void StatusLed::setRed(bool on) {
  setRedLevel(on ? 255 : 0);
}

void StatusLed::setWhite(bool on) {
  setWhiteLevel(on ? 255 : 0);
}

void StatusLed::blinkRedCount(int count) {
  for (int i = 0; i < count; i++) {
    setRed(true);  delay(160);
    setRed(false); delay(160);
  }
  delay(350);
}

void StatusLed::triggerButtonAck() {
  ackUntilMs_ = millis() + 90;
}

uint8_t StatusLed::breathDuty(uint32_t nowMs, uint32_t periodMs, uint8_t minDuty, uint8_t maxDuty) {
  float phase = static_cast<float>(nowMs % periodMs) / static_cast<float>(periodMs);
  float s     = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * phase)); // 0 -> 1 -> 0 smoothly
  float gamma = s * s; // perceptual gamma correction so low-end fade looks linear to the eye
  return static_cast<uint8_t>(minDuty + gamma * static_cast<float>(maxDuty - minDuty));
}

void StatusLed::update(const LedState &state) {
  uint32_t now = millis();

  // --- OTA UPDATE OVERRIDE: alternating smooth WHITE/RED pulse speeding up with progress ---
  if (state.otaActive) {
    uint32_t period = (state.otaProgressPct >= 95) ? 450 : 900;
    uint8_t whiteDuty = breathDuty(now, period, 15, 255);
    uint8_t redDuty   = breathDuty(now + (period / 2), period, 15, 220);
    setWhiteLevel(whiteDuty);
    setRedLevel(redDuty);
    return;
  }

  // --- WHITE LED: operating mode ---
  //   - Aux-In active              : steady soft white (35%)
  //   - Dual-Pair window open      : faster smooth white breath (1.0 s)
  //   - Bluetooth connected        : solid bright white (100%)
  //   - Pairing mode (unconnected) : slow smooth white breath (2.4 s)
  uint8_t whiteDuty = 0;
  if (state.auxActive) {
    whiteDuty = 90;
  } else if (state.dualPairOpen) {
    whiteDuty = breathDuty(now, 1000, 12, 255);
  } else if (state.btConnected) {
    whiteDuty = 255;
  } else {
    whiteDuty = breathDuty(now, 2400, 10, 255);
  }

  // Brief soft dip on front-button press for tactile visual feedback
  if (now < ackUntilMs_) {
    whiteDuty = (whiteDuty > 80) ? 40 : 220;
  }
  setWhiteLevel(whiteDuty);

  // --- RED LED: battery / charging ---
  //   - Charging (adapter + /CHG low) : slow smooth red breath (2.6 s)
  //   - Low battery (on battery)      : solid red warning
  //   - Normal / full                 : off
  uint8_t redDuty = 0;
  if (state.charging) {
    redDuty = breathDuty(now, 2600, 6, 255);
  } else if (state.lowBattery) {
    redDuty = 255;
  }
  setRedLevel(redDuty);
}

void StatusLed::updateStandbyCharging(bool charging) {
  setWhiteLevel(0);
  setRedLevel(charging ? breathDuty(millis(), 2600, 6, 255) : 0);
}

void StatusLed::haltWithError(int errorCode) {
  setWhiteLevel(0);
  for (;;) {
    for (int i = 0; i < errorCode; i++) {
      setRed(true);  delay(120);
      setRed(false); delay(120);
    }
    delay(900);
  }
}
