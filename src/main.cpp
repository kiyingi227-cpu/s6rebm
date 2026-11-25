/*
  HK Onyx Studio 5 — ESP32 Bluetooth Classic Speaker Firmware

  Subsystems:
    - board_config.h    : Pin map, polarities, timings, crossover & battery thresholds
    - power_manager.*   : SLG4V4826 latch (D4), real Power Button off (D17), wake (D15)
    - ak7755.*          : AK7755 I2C driver (DSP bypass SDIN1->DAC + Aux ADC->SDOUT3)
    - dsp_crossover.*   : 40 Hz subsonic HPF + 2.2 kHz LR4 2-way active crossover
    - audio_output.*    : Full-duplex I2S, TPA3116 amp/mute sequencing, Aux-In pump
    - bt_audio.*        : Classic BT A2DP Sink, AVRCP CT/TG, auto-reconnect & Dual-Pair
    - buttons.*         : VOL+/-, PLAY/PAUSE (1x/2x/3x), BLUE (dual-pair/swap/reset)
    - battery_monitor.* : BAT_DET, DC_DET, bq24090 /CHG, NTC thermal & cutoff guard
    - status_led.*      : Smooth PWM fading WHITE (mode/pairing) & RED (charging/low bat)

  LED Guide (hardware PWM smooth fade):
    - WHITE slow smooth fade   : Pairing mode (no device connected)
    - WHITE fast smooth fade   : Dual-Pair window open (60 s — pair a 2nd phone)
    - WHITE solid (100%)       : Bluetooth connected
    - WHITE soft steady (35%)  : Aux-In mode
    - RED slow smooth fade     : Charging (19 V adapter + bq24090 /CHG active)
    - RED solid                : Low battery warning (<= 3.35 V on battery)
    - RED repeating N blinks   : Fatal boot error (2..5 = AK7755, 6 = Bluetooth)
*/
#include <Arduino.h>

#include "audio_output.h"
#include "battery_monitor.h"
#include "board_config.h"
#include "bt_audio.h"
#include "bt_ota.h"
#include "buttons.h"
#include "power_manager.h"
#include "status_led.h"

using namespace BoardConfig;

static PowerManager   power;
static AudioOutput    audio;
static BtAudio        btAudio;
static BtOta          btOta;
static Buttons        buttons;
static BatteryMonitor battery;
static StatusLed      leds;
static bool           sysidMode = false;

static void enterSysidMode() {
  sysidMode = true;
  audio.setAutoAuxSense(false);
  audio.setDspMode(DspMode::FLAT_BASELINE);
  audio.setSampleRate(SYSID_SAMPLE_RATE); // Exact 48 kHz for SysID sweep capture
  audio.playPairChime();
}

static void exitSysidMode() {
  sysidMode = false;
  audio.setDspMode(DspMode::TUNED);
  audio.setAutoAuxSense(true);
  audio.playConnectedChime();
}

static void runSysidSweepWithCountdown() {
  // 3-second RED countdown so you can step clear of the microphone
  for (int i = 0; i < 3; i++) {
    leds.setRed(true);  delay(200);
    leds.setRed(false); delay(800);
  }
  leds.setRed(true);
  audio.playSysidSingleSweep();
  leds.setRed(false);
}

static void onWaitingForDsp() {
  leds.setRed(true);  delay(250);
  leds.setRed(false); delay(250);
}

[[noreturn]] static void fatalError(int code) {
  audio.disableAmps();
  leds.haltWithError(code);
}

[[noreturn]] static void performCleanShutdown() {
  btAudio.stop();
  delay(20);
  audio.playShutdownChime();
  audio.shutdownAll();
  leds.setRed(false);
  leds.setWhite(false);
  power.powerOffAndWaitForWake(leds);
}

static void handleButton(ButtonEvent ev) {
  if (ev == ButtonEvent::NONE) return;

  leds.triggerButtonAck();

  // Triple-tap BLUE (3x) OR hold BLUE + PLAY together (1.5 s) toggles 48 kHz SysID Mode
  if (ev == ButtonEvent::SYSID_TOGGLE || ev == ButtonEvent::BLUE_TRIPLE) {
    if (sysidMode) exitSysidMode();
    else           enterSysidMode();
    return;
  }

  // While in SysID Measurement Mode:
  //   BLUE (1x) OR PLAY/PAUSE (1x) -> 3 s RED countdown + exact -20 dBFS 48 kHz Farina sweep
  //   BLUE (2x)                    -> toggle FLAT_BASELINE (solid WHITE) <-> TUNED (breathing WHITE)
  //   BLUE (3x) or BLUE+PLAY hold  -> exit back to normal Bluetooth/Aux mode
  if (sysidMode) {
    if (ev == ButtonEvent::PLAY_PAUSE || ev == ButtonEvent::BLUE_SHORT) {
      runSysidSweepWithCountdown();
    } else if (ev == ButtonEvent::BLUE_DOUBLE) {
      DspMode next = (audio.dspMode() == DspMode::FLAT_BASELINE)
                         ? DspMode::TUNED
                         : DspMode::FLAT_BASELINE;
      audio.setDspMode(next);
      audio.playPairChime();
    }
    return;
  }

  switch (ev) {
    case ButtonEvent::VOL_UP:
      audio.adjustVolume(+VOLUME_STEP);
      btAudio.notifyVolumeChanged();
      break;

    case ButtonEvent::VOL_DOWN:
      audio.adjustVolume(-VOLUME_STEP);
      btAudio.notifyVolumeChanged();
      break;

    case ButtonEvent::PLAY_PAUSE:
      btAudio.sendPlayPause();
      break;

    case ButtonEvent::NEXT_TRACK:
      btAudio.sendNextTrack();
      break;

    case ButtonEvent::PREV_TRACK:
      btAudio.sendPrevTrack();
      break;

    case ButtonEvent::PLAY_LONG:
      if (audio.toggleManualAux()) {
        audio.playConnectedChime(); // Entered Aux-In (AK7755 pins 34/32)
      } else {
        audio.playPairChime();      // Returned to Bluetooth
      }
      break;

    case ButtonEvent::BLUE_SHORT:
      btAudio.openDualPairWindow();
      break;

    case ButtonEvent::BLUE_DOUBLE:
      btAudio.swapPairedDevice();
      break;

    case ButtonEvent::BLUE_LONG:
      btAudio.disconnectAndPair();
      break;

    default:
      break;
  }
}

void setup() {
  // 1. Put amps, AK7755, and LEDs into safe OFF states before deciding boot vs charging standby
  audio.initHardwareSafe();
  leds.begin();

  // If 19 V charger powered SYS_3V3 while the speaker is OFF (VREG_EN LOW, DC_DET HIGH, cold boot),
  // stay silent with amps/DSP/BT off and breathe the RED charging LED until Power Button is pressed.
  if (power.isChargerOnlyWake()) {
    power.waitForPowerButtonWithChargingLed(leds);
  }

  // 2. Latch SLG power ON immediately (within ~900 ms wake window) & init buttons/battery
  power.beginLatched();
  buttons.begin();
  battery.begin();

  // 2. Bring up AK7755 (bypass DAC + Aux ADC on pins 34/32) and full-duplex I2S on Core 1
  audio.setAutoAuxSense(true);
  AudioError err = audio.beginDspAndI2s(onWaitingForDsp);
  if (err != AudioError::OK) {
    fatalError(static_cast<int>(err));
  }
  leds.blinkRedCount(audio.i2cDevicesFound());

  // 3. Pop-free amp power-up & unmute, then play 2-driver verification chime
  audio.enableAmps();
  audio.playStartupChime();

  // 4. Start Bluetooth Classic A2DP + AVRCP (auto-reconnects or jumps straight to pairing)
  if (!btAudio.begin(audio)) {
    fatalError(6);
  }

  // 5. Start Dual-Mode Bluetooth OTA (BLE GATT + Classic SPP + brand discovery beacons)
  btOta.begin(audio, battery, btAudio);

  // 6. Boot succeeded cleanly — confirm running OTA partition to cancel rollback if pending
  btOta.confirmRunningFirmware();

  // 7. Arm real Power Button (SLG pin 4 -> D17) for clean shutdown
  power.armPowerOff();
}

void loop() {
  // 0. Handle deferred clean reboot after OTA commit
  if (btOta.rebootRequested()) {
    audio.shutdownAll();
    leds.setRed(false);
    leds.setWhite(false);
    delay(50);
    ESP.restart();
  }

  // 1. Real Power Button shutdown check (ONLY manual power-off active for now)
  if (power.pollPowerOffRequested()) {
    performCleanShutdown();
  }

  // 2. Process Dual-Mode Bluetooth OTA commands & flash writes on Core 1
  btOta.poll();

  // 3. Battery, charger & adapter readout (updates LEDs + 19 V AC vs 10 V Bat DSP ceilings)
  battery.poll();
  audio.setAcPower(battery.isDcPresent());

  // 4. Front buttons & AVRCP / Dual-Pair controls (paused while OTA is flashing)
  if (!btOta.isOtaActive()) {
    handleButton(buttons.poll());

    // 5. Aux-In jack detection (paused while in SysID capture mode)
    if (!sysidMode) {
      audio.pollAuxDetect();
    }
  }

  // 6. Bluetooth deferred events (re-clocking, DMA silence, dual-pair handover)
  btAudio.poll();

  // 7. Smooth hardware-PWM LED animations
  LedState ledState;
  if (sysidMode) {
    // SysID capture LED guide: solid WHITE = FLAT_BASELINE (P6), breathing WHITE = TUNED
    ledState.btConnected  = (audio.dspMode() == DspMode::FLAT_BASELINE);
    ledState.dualPairOpen = false;
    ledState.auxActive    = false;
  } else {
    ledState.btConnected  = btAudio.isConnected();
    ledState.dualPairOpen = btAudio.isDualPairOpen();
    ledState.auxActive    = audio.isAuxActive();
  }
  ledState.charging       = battery.isCharging();
  ledState.lowBattery     = battery.isLowBattery();
  ledState.otaActive      = btOta.isOtaActive();
  ledState.otaProgressPct = btOta.otaProgressPct();
  leds.update(ledState);

  delay(btOta.isOtaActive() ? 1 : 15);
}
