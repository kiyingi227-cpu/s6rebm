#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ak7755.h"
#include "chime_data.h"
#include "dsp_crossover.h"

enum class AudioError : int {
  OK          = 0,
  CONFIGURE   = 2,
  CLOCK_RESET = 3,
  CODEC_RESET = 4,
  VERIFY      = 5
};

class AudioOutput {
public:
  // Sets safe mute/amp/DSP states at boot
  void initHardwareSafe();

  // Initializes I2C, waits for AK7755, configures bypass + Aux ADC, starts I2S & Core 1 DSP task
  AudioError beginDspAndI2s(void (*onWaitingTick)(), uint32_t initialSampleRate = 44100);

  // Pop-free TPA3116 power-up and datasheet-compliant shutdown sequence
  void enableAmps();
  void disableAmps();
  void shutdownAll();
  void setOtaMode(bool active);
  bool isOtaMode() const { return otaMode_; }

  // Soft 2-driver chimes & Part D SysID single sweep (-20 dBFS Farina ESS, identical to WAV)
  void playStartupChime();
  void playShutdownChime();
  void playPairChime();
  void playConnectedChime();
  void playSysidSingleSweep();

  // Non-blocking enqueue called from Bluedroid A2DP callback (Core 0) -> processed on Core 1
  void writeStereoPcm16(const int16_t *stereoIn, size_t frames);
  void setSampleRate(uint32_t newRate);
  void clearDma();

  // Aux-In (AK7755 pins 34/32 IN1/IN3 -> stereo ADC -> SDOUT3 -> ESP32 D27 -> Core 1 DSP)
  // Supports jack detect (D35), auto audio-signal sense, and manual toggle (PLAY/PAUSE hold).
  bool pollAuxDetect();
  void setAutoAuxSense(bool enabled) { autoAuxSense_ = enabled; }
  bool toggleManualAux();
  bool isAuxActive() const { return auxActive_; }

  uint32_t sampleRate() const { return currentRate_; }
  void     setVolume(uint8_t avrcpVol) { crossover_.setVolume(avrcpVol); }
  void     setAcPower(bool acPower) { crossover_.setAcPower(acPower); }
  void     setDspMode(DspMode mode) { crossover_.setMode(mode); }
  DspMode  dspMode() const { return crossover_.mode(); }
  uint8_t  adjustVolume(int delta);
  uint8_t  volume() const { return crossover_.volume(); }
  int      i2cDevicesFound() const { return i2cCount_; }

private:
  // Lock-free SPSC ring buffer between Core 0 (Bluetooth A2DP) and Core 1 (DSP + I2S)
  static constexpr size_t RING_FRAMES = 4096; // Power of two (~93 ms @ 44.1 kHz, 16 KB)
  static constexpr size_t RING_MASK   = RING_FRAMES - 1;
  static constexpr size_t DSP_CHUNK   = 128;

  void startI2s(uint32_t rate);
  void startDspTask();
  void pauseDspTask();
  void resumeDspTask();
  void flushRing();

  static void dspTaskEntry(void *arg);
  void dspTaskLoop();
  void updateAuxActiveFlag();
  void pumpAuxChunk();
  void senseAuxIdleChunk();
  void playChannelTone(int channel, float freqHz, int durationMs, float amplitude);
  void playAdpcmChime(const ChimeData::AdpcmClip &clip);

  Ak7755         dsp_;
  AudioCrossover crossover_;
  uint32_t       currentRate_   = 44100;
  int            i2cCount_      = 0;
  bool           ampsEnabled_   = false;

  // Dedicated Core 1 DSP task & synchronization
  TaskHandle_t      dspTaskHandle_ = nullptr;
  volatile bool     pauseReq_      = false;
  volatile bool     dspPaused_     = false;
  volatile bool     shuttingDown_  = false;
  volatile uint32_t ringHead_      = 0;
  volatile uint32_t ringTail_      = 0;
  int16_t           ringBuf_[RING_FRAMES * 2] = {};

  // Aux-In state (jack detect + auto signal sense + manual override)
  int            auxIdleLevel_     = 0;
  bool           auxLastRaw_       = false;
  uint32_t       auxChangedMs_     = 0;
  volatile bool  autoAuxSense_     = false;
  volatile bool  auxJackActive_    = false;
  volatile bool  auxSignalActive_  = false;
  volatile bool  auxManualActive_  = false;
  volatile bool  auxActive_        = false;
  uint32_t       auxSenseStartMs_  = 0;
  uint32_t       auxLastSignalMs_  = 0;
  volatile bool  otaMode_          = false;
};
