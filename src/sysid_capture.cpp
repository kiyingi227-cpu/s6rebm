/*
  HK Onyx Studio 5 — 48 kHz Acoustic SysID Capture & Validation Firmware (Part D)

  Dedicated measurement mode (no Bluetooth RF / SBC artifacts, exact 48 kHz APLL clock,
  documented AK7755 CONT14 = 0xAC: -6 dB woofer / -10 dB tweeter, fixed 0 dB digital gain).

  Controls:
    - PLAY/PAUSE (D18) : 3-second RED countdown -> plays exact ess_single_m20db sweep
                         (1.0 s silence -> 3x50 ms 1 kHz marker -> 1.0 s -> 4 s Farina ESS
                         20 Hz..20 kHz @ -20 dBFS -> 1.0 s silence)
    - BLUE (D32)       : Toggles between FLAT_BASELINE (P6 capture) and TUNED (validation)
                         without moving the speaker!
    - Real Power Key   : Clean pop-free shutdown (SLG pin 4 -> D17)

  LEDs:
    - Solid WHITE      : FLAT_BASELINE mode (mandatory 48 Hz 4th-order HPF + 3.5 kHz LR4 only)
    - Breathing WHITE  : TUNED mode (eq_coeffs.h / Harman DSP active for validation sweep)
    - 3 RED blinks     : 3-second countdown before sweep starts (step away from mic!)
    - Solid RED        : Sweep currently playing
*/
#include <Arduino.h>

#include "audio_output.h"
#include "battery_monitor.h"
#include "board_config.h"
#include "buttons.h"
#include "power_manager.h"
#include "status_led.h"

using namespace BoardConfig;

static PowerManager   power;
static AudioOutput    audio;
static Buttons        buttons;
static BatteryMonitor battery;
static StatusLed      leds;

static void onWaitingForDsp() {
  leds.setRed(true);  delay(250);
  leds.setRed(false); delay(250);
}

[[noreturn]] static void fatalError(int code) {
  audio.disableAmps();
  leds.haltWithError(code);
}

[[noreturn]] static void performCleanShutdown() {
  audio.playShutdownChime();
  audio.shutdownAll();
  leds.setRed(false);
  leds.setWhite(false);
  power.powerOffAndWaitForWake(leds);
}

static void runSweepWithCountdown() {
  // 3-second countdown so you can step clear of the microphone
  for (int i = 0; i < 3; i++) {
    leds.setRed(true);  delay(200);
    leds.setRed(false); delay(800);
  }
  leds.setRed(true);
  audio.playSysidSingleSweep();
  leds.setRed(false);
}

void setup() {
  power.beginLatched();
  audio.initHardwareSafe();
  leds.begin();
  buttons.begin();
  battery.begin();

  // Start in FLAT_BASELINE mode at exact 48 kHz (Part D specification)
  audio.setDspMode(DspMode::FLAT_BASELINE);

  AudioError err = audio.beginDspAndI2s(onWaitingForDsp, SYSID_SAMPLE_RATE);
  if (err != AudioError::OK) {
    fatalError(static_cast<int>(err));
  }
  leds.blinkRedCount(audio.i2cDevicesFound());

  audio.enableAmps();
  audio.playStartupChime();

  power.armPowerOff();
}

void loop() {
  if (power.pollPowerOffRequested()) {
    performCleanShutdown();
  }

  battery.poll();
  audio.setAcPower(battery.isDcPresent());

  ButtonEvent ev = buttons.poll();
  if (ev == ButtonEvent::PLAY_PAUSE) {
    runSweepWithCountdown();
  } else if (ev == ButtonEvent::BLUE_SHORT) {
    DspMode next = (audio.dspMode() == DspMode::FLAT_BASELINE)
                       ? DspMode::TUNED
                       : DspMode::FLAT_BASELINE;
    audio.setDspMode(next);
    leds.triggerButtonAck();
    audio.playPairChime();
  }

  // LEDs: solid WHITE = FLAT_BASELINE (P6), breathing WHITE = TUNED (validation)
  LedState st;
  st.btConnected  = (audio.dspMode() == DspMode::FLAT_BASELINE);
  st.dualPairOpen = false;
  st.auxActive    = false;
  st.charging     = battery.isCharging();
  st.lowBattery   = battery.isLowBattery();
  leds.update(st);

  delay(15);
}
