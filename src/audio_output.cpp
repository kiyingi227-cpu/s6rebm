#include "audio_output.h"
#include "board_config.h"

#include <Wire.h>
#include <math.h>
#include <string.h>
#include "driver/i2s.h"

using namespace BoardConfig;

namespace {
constexpr BaseType_t  DSP_TASK_CORE     = 1;    // Dedicated APP_CPU core (Bluetooth stack runs on Core 0)
constexpr UBaseType_t DSP_TASK_PRIORITY = 5;    // Above Arduino loop() (prio 1), smooth real-time audio
constexpr uint32_t    DSP_TASK_STACK    = 6144; // Generous stack for double-precision Biquad64 + I2S

constexpr int8_t kImaIndexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

constexpr int16_t kImaStepTable[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,
     16,    17,    19,    21,    23,    25,    28,    31,
     34,    37,    41,    45,    50,    55,    60,    66,
     73,    80,    88,    97,   107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,  1060,  1166,  1282,  1411,
   1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
   3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
   7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767
};

struct ImaAdpcmState {
  int32_t predictor = 0;
  int32_t stepIndex = 0;

  inline float decodeNibble(uint8_t code) {
    int32_t step   = kImaStepTable[stepIndex];
    int32_t vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;

    if (code & 8) predictor -= vpdiff;
    else          predictor += vpdiff;

    if (predictor > 32767)       predictor = 32767;
    else if (predictor < -32768) predictor = -32768;

    stepIndex += kImaIndexTable[code & 0x0F];
    if (stepIndex < 0)       stepIndex = 0;
    else if (stepIndex > 88) stepIndex = 88;

    return static_cast<float>(predictor) * (1.0f / 32768.0f);
  }
};

// 4-point Catmull-Rom cubic interpolation (smooth upsampling from 22.05 kHz -> 32/44.1/48 kHz)
inline float catmullRom(float y0, float y1, float y2, float y3, float t) {
  float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
  float a1 =         y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  float a2 = -0.5f * y0             + 0.5f * y2;
  float a3 =                     y1;
  return ((a0 * t + a1) * t + a2) * t + a3;
}
} // namespace

void AudioOutput::initHardwareSafe() {
  pinMode(PIN_MUTE, OUTPUT);
  digitalWrite(PIN_MUTE, MUTE_ON);

  pinMode(PIN_AMP_PWR, OUTPUT);
  digitalWrite(PIN_AMP_PWR, AMP_OFF);

  dsp_.beginPins();

  pinMode(PIN_I2S_DIN, INPUT);
  pinMode(PIN_SHDN, INPUT);
  pinMode(PIN_AUX_IN_DET, INPUT);

  currentRate_ = DEFAULT_SAMPLE_RATE;
  crossover_.setVolume(DEFAULT_BT_VOLUME);
  crossover_.configure(currentRate_);
}

AudioError AudioOutput::beginDspAndI2s(void (*onWaitingTick)(), uint32_t initialSampleRate) {
  currentRate_ = initialSampleRate;
  crossover_.configure(currentRate_);

  delay(RAIL_WAIT_MS);
  dsp_.releasePowerDown();

  Wire.begin(PIN_SDA, PIN_SCL, I2C_CLOCK_HZ);
  i2cCount_ = dsp_.scanBus();

  while (!dsp_.isFound()) {
    if (onWaitingTick) onWaitingTick();
    dsp_.releasePowerDown();
    i2cCount_ = dsp_.scanBus();
  }

  if (!dsp_.configureBypass(AK_WOOFER_VOL, AK_TWEETER_VOL, currentRate_)) {
    return AudioError::CONFIGURE;
  }

  startI2s(currentRate_);
  delay(I2S_CLOCK_STABLE_MS);

  if (!dsp_.releaseClockReset()) return AudioError::CLOCK_RESET;
  delay(PLL_LOCK_WAIT_MS);

  if (!dsp_.releaseCodecReset()) return AudioError::CODEC_RESET;
  delay(CODEC_SETTLE_MS);

  if (!dsp_.verify()) return AudioError::VERIFY;

  // Learn Aux-In jack idle level after AK7755 is initialized so static levels never false-trigger
  auxIdleLevel_    = digitalRead(PIN_AUX_IN_DET);
  auxLastRaw_      = false;
  auxJackActive_   = false;
  auxSignalActive_ = false;
  auxManualActive_ = false;
  auxActive_       = false;
  auxChangedMs_    = millis();

  // Launch dedicated Core 1 DSP + I2S processing task
  startDspTask();
  return AudioError::OK;
}

void AudioOutput::startI2s(uint32_t rate) {
  i2s_config_t cfg = {};
  cfg.mode                 = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate          = rate;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = true;
  cfg.tx_desc_auto_clear   = true;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_LRCLK;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num  = PIN_I2S_DIN;
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

void AudioOutput::startDspTask() {
  if (dspTaskHandle_ != nullptr) return;
  shuttingDown_ = false;
  pauseReq_     = false;
  dspPaused_    = false;
  ringHead_     = 0;
  ringTail_     = 0;
  xTaskCreatePinnedToCore(
      dspTaskEntry,
      "dsp_core1",
      DSP_TASK_STACK,
      this,
      DSP_TASK_PRIORITY,
      &dspTaskHandle_,
      DSP_TASK_CORE);
}

void AudioOutput::pauseDspTask() {
  if (dspTaskHandle_ == nullptr) return;
  pauseReq_ = true;
  xTaskNotifyGive(dspTaskHandle_);
  uint32_t t0 = millis();
  while (!dspPaused_ && (millis() - t0) < 150) {
    delay(1);
  }
}

void AudioOutput::resumeDspTask() {
  if (dspTaskHandle_ == nullptr) return;
  pauseReq_ = false;
  xTaskNotifyGive(dspTaskHandle_);
}

void AudioOutput::flushRing() {
  ringTail_ = ringHead_;
  __sync_synchronize();
}

void AudioOutput::dspTaskEntry(void *arg) {
  static_cast<AudioOutput *>(arg)->dspTaskLoop();
}

void AudioOutput::updateAuxActiveFlag() {
  auxActive_ = auxManualActive_ || auxJackActive_ || auxSignalActive_;
}

void AudioOutput::dspTaskLoop() {
  int16_t inChunk[DSP_CHUNK * 2];
  int32_t outChunk[DSP_CHUNK * 2];

  for (;;) {
    if (shuttingDown_) {
      dspPaused_ = true;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (pauseReq_) {
      dspPaused_ = true;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    dspPaused_ = false;

    uint32_t head  = ringHead_;
    __sync_synchronize();
    uint32_t tail  = ringTail_;
    uint32_t avail = head - tail;

    // If Bluetooth starts streaming while we were only in auto-sense Aux, yield back to BT
    if (avail > 0 && auxSignalActive_ && !auxManualActive_ && !auxJackActive_) {
      auxSignalActive_ = false;
      auxSenseStartMs_ = 0;
      updateAuxActiveFlag();
    }

    // Aux-In mode (AK7755 pins 34/32 IN1/IN3 -> ADC -> SDOUT3 -> D27 -> Core 1 DSP -> I2S TX)
    if (auxActive_) {
      pumpAuxChunk();
      continue;
    }

    // Bluetooth A2DP mode: pop stereo PCM frames from lock-free ring buffer -> Core 1 DSP -> I2S TX
    if (avail == 0) {
      if (autoAuxSense_) {
        senseAuxIdleChunk();
      } else {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
      }
      continue;
    }

    size_t n = (avail > DSP_CHUNK) ? DSP_CHUNK : static_cast<size_t>(avail);
    size_t startIdx = tail & RING_MASK;
    size_t firstPart = RING_FRAMES - startIdx;
    if (firstPart >= n) {
      memcpy(inChunk, &ringBuf_[startIdx * 2], n * 4);
    } else {
      memcpy(inChunk, &ringBuf_[startIdx * 2], firstPart * 4);
      memcpy(inChunk + firstPart * 2, &ringBuf_[0], (n - firstPart) * 4);
    }

    __sync_synchronize();
    ringTail_ = tail + static_cast<uint32_t>(n);

    crossover_.processBlock(inChunk, outChunk, n);

    size_t written = 0;
    i2s_write(I2S_NUM_0, outChunk, n * 8, &written, pdMS_TO_TICKS(100));
  }
}

void AudioOutput::senseAuxIdleChunk() {
  int32_t rxBuf[DSP_CHUNK * 2];
  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, rxBuf, sizeof(rxBuf), &bytesRead, pdMS_TO_TICKS(8)) != ESP_OK ||
      bytesRead < 8) {
    return;
  }

  int32_t peak = 0;
  size_t count = bytesRead / 4;
  for (size_t i = 0; i < count; i++) {
    int32_t s = rxBuf[i] >> 16;
    if (s < 0) s = -s;
    if (s > peak) peak = s;
  }

  uint32_t now = millis();
  if (peak >= AUX_SENSE_PEAK_THR) {
    if (auxSenseStartMs_ == 0) {
      auxSenseStartMs_ = now;
    } else if ((now - auxSenseStartMs_) >= AUX_SENSE_HOLD_MS) {
      auxSignalActive_ = true;
      auxLastSignalMs_ = now;
      updateAuxActiveFlag();
    }
  } else {
    auxSenseStartMs_ = 0;
  }
}

void AudioOutput::pumpAuxChunk() {
  int32_t rxBuf[DSP_CHUNK * 2];
  int16_t pcm16[DSP_CHUNK * 2];
  int32_t txBuf[DSP_CHUNK * 2];

  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, rxBuf, sizeof(rxBuf), &bytesRead, pdMS_TO_TICKS(15)) != ESP_OK ||
      bytesRead < 8) {
    return;
  }

  size_t frames = bytesRead / 8;
  int32_t peak  = 0;
  for (size_t i = 0; i < frames * 2; i++) {
    int16_t s = static_cast<int16_t>(rxBuf[i] >> 16);
    pcm16[i] = s;
    int32_t a = (s < 0) ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
    if (a > peak) peak = a;
  }

  // Track signal activity for auto-sense release
  uint32_t now = millis();
  if (peak >= AUX_SENSE_PEAK_THR) {
    auxLastSignalMs_ = now;
  } else if (auxSignalActive_ && !auxManualActive_ && !auxJackActive_ &&
             (now - auxLastSignalMs_) >= AUX_SILENCE_EXIT_MS) {
    auxSignalActive_ = false;
    auxSenseStartMs_ = 0;
    updateAuxActiveFlag();
    i2s_zero_dma_buffer(I2S_NUM_0);
    return;
  }

  crossover_.processBlock(pcm16, txBuf, frames);

  size_t written = 0;
  i2s_write(I2S_NUM_0, txBuf, frames * 8, &written, pdMS_TO_TICKS(25));
}

void AudioOutput::enableAmps() {
  digitalWrite(PIN_AMP_PWR, AMP_ON);
  delay(AMP_SETTLE_MS);
  digitalWrite(PIN_MUTE, MUTE_OFF);
  delay(UNMUTE_SETTLE_MS);
  dsp_.setSoftMute(false);
  delay(DAMUTE_RAMP_MS);
  ampsEnabled_ = true;
}

void AudioOutput::disableAmps() {
  if (ampsEnabled_ && dsp_.isFound()) {
    dsp_.setSoftMute(true);
    delay(DAMUTE_RAMP_MS);
  }
  digitalWrite(PIN_MUTE, MUTE_ON);
  delay(UNMUTE_SETTLE_MS);
  digitalWrite(PIN_AMP_PWR, AMP_OFF);
  ampsEnabled_ = false;
}

void AudioOutput::shutdownAll() {
  shuttingDown_ = true;
  pauseDspTask();
  flushRing();
  disableAmps();
  delay(50);
  dsp_.holdPowerDown();
  delay(10);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_stop(I2S_NUM_0);
}

void AudioOutput::clearDma() {
  pauseDspTask();
  flushRing();
  i2s_zero_dma_buffer(I2S_NUM_0);
  resumeDspTask();
}

void AudioOutput::setOtaMode(bool active) {
  if (active == otaMode_) return;
  otaMode_ = active;
  if (active) {
    if (ampsEnabled_ && dsp_.isFound()) {
      dsp_.setSoftMute(true);
      delay(DAMUTE_RAMP_MS);
    }
    digitalWrite(PIN_MUTE, MUTE_ON);
    pauseDspTask();
    flushRing();
    i2s_zero_dma_buffer(I2S_NUM_0);
  } else {
    flushRing();
    resumeDspTask();
    if (ampsEnabled_) {
      digitalWrite(PIN_MUTE, MUTE_OFF);
      delay(UNMUTE_SETTLE_MS);
      if (dsp_.isFound()) {
        dsp_.setSoftMute(false);
      }
    }
  }
}

uint8_t AudioOutput::adjustVolume(int delta) {
  int v = static_cast<int>(crossover_.volume()) + delta;
  if (v < 0)   v = 0;
  if (v > 127) v = 127;
  crossover_.setVolume(static_cast<uint8_t>(v));
  return crossover_.volume();
}

void AudioOutput::setSampleRate(uint32_t newRate) {
  if (newRate != 44100 && newRate != 48000 && newRate != 32000) return;
  if (newRate == currentRate_) return;

  pauseDspTask();
  flushRing();

  dsp_.setSoftMute(true);
  delay(DAMUTE_RAMP_MS);

  dsp_.enterClockResetForRate(newRate);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_set_clk(I2S_NUM_0, newRate, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
  crossover_.configure(newRate);
  currentRate_ = newRate;
  delay(20);

  dsp_.releaseClockReset();
  delay(PLL_LOCK_WAIT_MS);
  dsp_.releaseCodecReset();
  delay(25);
  dsp_.setSoftMute(false);
  delay(DAMUTE_RAMP_MS);

  resumeDspTask();
}

// Called from Bluedroid A2DP callback (Core 0): fast lock-free enqueue, zero DSP math on Core 0!
void AudioOutput::writeStereoPcm16(const int16_t *stereoIn, size_t frames) {
  // Drop BT PCM only if Aux-In was explicitly locked via jack detect or manual toggle
  if (shuttingDown_ || pauseReq_ || auxManualActive_ || auxJackActive_ || frames == 0) return;

  uint32_t head = ringHead_;
  uint32_t tail = ringTail_;
  __sync_synchronize();

  uint32_t freeFrames = RING_FRAMES - (head - tail);
  size_t toWrite = (frames <= freeFrames) ? frames : static_cast<size_t>(freeFrames);
  if (toWrite == 0) return;

  size_t startIdx = head & RING_MASK;
  size_t firstPart = RING_FRAMES - startIdx;
  if (firstPart >= toWrite) {
    memcpy(&ringBuf_[startIdx * 2], stereoIn, toWrite * 4);
  } else {
    memcpy(&ringBuf_[startIdx * 2], stereoIn, firstPart * 4);
    memcpy(&ringBuf_[0], stereoIn + firstPart * 2, (toWrite - firstPart) * 4);
  }

  __sync_synchronize();
  ringHead_ = head + static_cast<uint32_t>(toWrite);

  if (dspTaskHandle_ != nullptr) {
    xTaskNotifyGive(dspTaskHandle_);
  }
}

bool AudioOutput::pollAuxDetect() {
  uint32_t now = millis();
  bool raw = (digitalRead(PIN_AUX_IN_DET) != auxIdleLevel_);
  if (raw != auxLastRaw_) {
    auxLastRaw_   = raw;
    auxChangedMs_ = now;
  }
  if ((now - auxChangedMs_) >= 250 && raw != auxJackActive_) {
    pauseDspTask();
    flushRing();
    i2s_zero_dma_buffer(I2S_NUM_0);
    auxJackActive_ = raw;
    if (!raw) {
      auxSignalActive_ = false;
      auxManualActive_ = false;
    }
    updateAuxActiveFlag();
    resumeDspTask();
    return true;
  }
  return false;
}

bool AudioOutput::toggleManualAux() {
  pauseDspTask();
  flushRing();
  i2s_zero_dma_buffer(I2S_NUM_0);
  if (auxActive_) {
    // Force exit Aux-In back to Bluetooth
    auxManualActive_ = false;
    auxSignalActive_ = false;
    auxJackActive_   = false;
  } else {
    // Force enter Aux-In immediately (AK7755 IN1 pin 34 & IN3 pin 32)
    auxManualActive_ = true;
    auxLastSignalMs_ = millis();
  }
  updateAuxActiveFlag();
  resumeDspTask();
  return auxActive_;
}

void AudioOutput::playChannelTone(int channel, float freqHz, int durationMs, float amplitude) {
  pauseDspTask();
  flushRing();

  static constexpr int N = 128;
  int32_t buf[N * 2];
  float phase = 0.0f;
  float step  = 2.0f * static_cast<float>(M_PI) * freqHz / static_cast<float>(currentRate_);
  uint32_t total = currentRate_ * durationMs / 1000;
  uint32_t fade  = currentRate_ * 20 / 1000;
  uint32_t done  = 0;

  while (done < total) {
    for (int i = 0; i < N; i++) {
      uint32_t pos = done + i;
      float env = 1.0f;
      if (pos < fade) {
        env = static_cast<float>(pos) / static_cast<float>(fade);
      } else if (pos + fade > total) {
        env = (total > pos) ? static_cast<float>(total - pos) / static_cast<float>(fade) : 0.0f;
      }

      int32_t s = static_cast<int32_t>(sinf(phase) * amplitude * env * 2147483647.0f);
      phase += step;
      if (phase > 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);

      int32_t wf = (channel == 0) ? s : 0;
      int32_t tw = (channel == 1) ? s : 0;
      if (SWAP_WOOFER_TWEETER) {
        buf[2 * i]     = tw;
        buf[2 * i + 1] = wf;
      } else {
        buf[2 * i]     = wf;
        buf[2 * i + 1] = tw;
      }
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    done += N;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  resumeDspTask();
}

void AudioOutput::playAdpcmChime(const ChimeData::AdpcmClip &clip) {
  if (!clip.data || clip.sampleCount == 0) return;

  pauseDspTask();
  flushRing();

  static constexpr size_t CHUNK = 128;
  float   mono[CHUNK];
  int32_t out[CHUNK * 2];

  ImaAdpcmState dec;
  uint32_t      srcIndex = 0;

  auto nextSrcSample = [&]() -> float {
    if (srcIndex >= clip.sampleCount) return 0.0f;
    uint8_t byteVal = clip.data[srcIndex >> 1];
    uint8_t nibble  = (srcIndex & 1U) ? (byteVal >> 4) : (byteVal & 0x0FU);
    srcIndex++;
    return dec.decodeNibble(nibble);
  };

  // Prime 4-point Catmull-Rom window [y0, y1, y2, y3] centered between y1 and y2
  float y0 = 0.0f;
  float y1 = nextSrcSample();
  float y2 = nextSrcSample();
  float y3 = nextSrcSample();

  const double   step       = static_cast<double>(ChimeData::SAMPLE_RATE_HZ) /
                              static_cast<double>(currentRate_);
  const uint32_t totalOut   = static_cast<uint32_t>(
      llround(static_cast<double>(clip.sampleCount) / step));
  const uint32_t tailFlush  = (currentRate_ * 45U) / 1000U; // 45 ms DSP & look-ahead flush
  double         phase      = 0.0;
  uint32_t       writtenOut = 0;

  while (writtenOut < totalOut + tailFlush) {
    size_t n = 0;
    while (n < CHUNK && writtenOut < totalOut + tailFlush) {
      if (writtenOut < totalOut) {
        float t = static_cast<float>(phase);
        mono[n] = catmullRom(y0, y1, y2, y3, t) * CHIME_GAIN;
        phase += step;
        while (phase >= 1.0) {
          phase -= 1.0;
          y0 = y1;
          y1 = y2;
          y2 = y3;
          y3 = nextSrcSample();
        }
      } else {
        mono[n] = 0.0f;
      }
      n++;
      writtenOut++;
    }

    crossover_.processMonoFloatBlock(mono, out, n);
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, out, n * 8, &bytesWritten, portMAX_DELAY);
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  resumeDspTask();
}

void AudioOutput::playStartupChime() {
  playAdpcmChime(ChimeData::POWER_ON);
}

void AudioOutput::playShutdownChime() {
  playAdpcmChime(ChimeData::POWER_OFF);
}

void AudioOutput::playPairChime() {
  playAdpcmChime(ChimeData::PAIRING);
}

void AudioOutput::playConnectedChime() {
  playChannelTone(1, 3000.0f, 110, 0.04f);
}

void AudioOutput::playSysidSingleSweep() {
  // Exact match to tools/generate_signals.py ess_single_m20db.wav:
  //   1.0 s silence -> 3x50 ms 1 kHz -12 dBFS marker -> 1.0 s silence ->
  //   4.0 s Farina ESS (20 Hz..20 kHz, -20 dBFS, 10 ms cosine fades) -> 1.0 s silence
  // Uses a 1024-entry interpolated sine LUT + trapezoidal phase recurrence (108 dB SNR,
  // zero per-sample exp()/sin() calls) so ESP32 never underruns I2S DMA at 48 kHz.
  pauseDspTask();
  flushRing();
  i2s_zero_dma_buffer(I2S_NUM_0);

  static constexpr size_t CHUNK   = 256;
  static constexpr int    LUT_N   = 1024;
  static float            sinLut[LUT_N + 1];
  static bool             lutInit = false;
  if (!lutInit) {
    for (int i = 0; i <= LUT_N; i++) {
      sinLut[i] = sinf(2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / static_cast<float>(LUT_N));
    }
    lutInit = true;
  }

  float   mono[CHUNK];
  int32_t out[CHUNK * 2];

  const double fs = static_cast<double>(currentRate_);
  auto writeMonoChunk = [&](size_t count) {
    crossover_.processMonoFloatBlock(mono, out, count);
    size_t written = 0;
    i2s_write(I2S_NUM_0, out, count * 8, &written, portMAX_DELAY);
  };

  auto sendSilence = [&](double seconds) {
    size_t total = static_cast<size_t>(llround(seconds * fs));
    for (size_t i = 0; i < CHUNK; i++) mono[i] = 0.0f;
    size_t sent = 0;
    while (sent < total) {
      size_t n = (total - sent > CHUNK) ? CHUNK : (total - sent);
      writeMonoChunk(n);
      sent += n;
    }
  };

  // 1. Leading silence (1.0 s)
  sendSilence(1.0);

  // 2. Start marker: 3 x 50 ms bursts of 1 kHz @ -12 dBFS, 5 ms cosine fades, 50 ms gaps
  {
    const float  amp     = powf(10.0f, -12.0f / 20.0f);
    const size_t burstN  = static_cast<size_t>(llround(0.050 * fs));
    const size_t fadeN   = static_cast<size_t>(llround(0.005 * fs));
    const float  invFade = static_cast<float>(M_PI) / static_cast<float>(fadeN);
    const double stepLut = static_cast<double>(LUT_N) * 1000.0 / fs;

    for (int b = 0; b < 3; b++) {
      double phi = 0.0;
      size_t idx = 0;
      while (idx < burstN) {
        size_t n = (burstN - idx > CHUNK) ? CHUNK : (burstN - idx);
        for (size_t i = 0; i < n; i++) {
          size_t pos = idx + i;
          float  w   = 1.0f;
          if (pos < fadeN) {
            w = 0.5f * (1.0f - cosf(invFade * static_cast<float>(pos)));
          } else if (pos + fadeN >= burstN) {
            size_t tail = burstN - 1 - pos;
            w = 0.5f * (1.0f - cosf(invFade * static_cast<float>(tail)));
          }
          int   k    = static_cast<int>(phi);
          float frac = static_cast<float>(phi - static_cast<double>(k));
          float s    = sinLut[k] + frac * (sinLut[k + 1] - sinLut[k]);
          mono[i]    = amp * w * s;
          phi += stepLut;
          if (phi >= static_cast<double>(LUT_N)) phi -= static_cast<double>(LUT_N);
        }
        writeMonoChunk(n);
        idx += n;
      }
      if (b < 2) sendSilence(0.050);
    }
  }

  // 3. Pre-sweep silence (1.0 s)
  sendSilence(1.0);

  // 4. Farina Exponential Sine Sweep: exact trapezoidal phase recurrence + LUT (zero exp/sin)
  {
    const float  amp     = powf(10.0f, SYSID_SWEEP_LEVEL_DBFS / 20.0f);
    const double durS    = static_cast<double>(SYSID_SWEEP_DUR_S);
    const double f1      = static_cast<double>(SYSID_SWEEP_F1_HZ);
    const double f2      = static_cast<double>(SYSID_SWEEP_F2_HZ);
    const size_t total   = static_cast<size_t>(llround(durS * fs));
    const size_t fadeN   = static_cast<size_t>(llround(static_cast<double>(SYSID_SWEEP_FADE_MS) * 1e-3 * fs));
    const float  invFade = static_cast<float>(M_PI) / static_cast<float>(fadeN);

    const double r       = pow(f2 / f1, 1.0 / static_cast<double>(total));
    const double lutD    = static_cast<double>(LUT_N);
    double       dphi    = lutD * f1 / fs;
    double       phi     = 0.0;

    size_t idx = 0;
    while (idx < total) {
      size_t n = (total - idx > CHUNK) ? CHUNK : (total - idx);
      for (size_t i = 0; i < n; i++) {
        size_t pos = idx + i;
        float  w   = 1.0f;
        if (pos < fadeN) {
          w = 0.5f * (1.0f - cosf(invFade * static_cast<float>(pos)));
        } else if (pos + fadeN >= total) {
          size_t tail = total - 1 - pos;
          w = 0.5f * (1.0f - cosf(invFade * static_cast<float>(tail)));
        }

        int   k    = static_cast<int>(phi);
        float frac = static_cast<float>(phi - static_cast<double>(k));
        float s    = sinLut[k] + frac * (sinLut[k + 1] - sinLut[k]);
        mono[i]    = amp * w * s;

        double nextDphi = dphi * r;
        phi += 0.5 * (dphi + nextDphi);
        if (phi >= lutD) phi -= lutD;
        dphi = nextDphi;
      }
      writeMonoChunk(n);
      idx += n;
    }
  }

  // 5. Trailing silence (1.0 s) + clear DMA
  sendSilence(1.0);
  i2s_zero_dma_buffer(I2S_NUM_0);
  resumeDspTask();
}
