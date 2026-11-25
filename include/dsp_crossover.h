#pragma once
#include <stddef.h>
#include <stdint.h>

// Single-precision Transposed Direct Form II (TDF-II) biquad filter.
// Updating coefficients via set*() preserves filter state (z1_, z2_) for
// click-free runtime modulation; call reset() on init or sample-rate change.
class Biquad {
public:
  void reset();
  void setBypass();
  void setCoeffs(float b0, float b1, float b2, float a1, float a2);
  void setLPF(float fc, float fs, float Q);
  void setHPF(float fc, float fs, float Q);
  void setButterworthLPF(float fc, float fs);
  void setButterworthHPF(float fc, float fs);
  void setPeakingEQ(float fc, float fs, float Q, float gainDb);
  void setLowShelf(float fc, float fs, float slope, float gainDb);
  void setHighShelf(float fc, float fs, float slope, float gainDb);
  void setAllPass(float fc, float fs, float Q);

  inline float process(float x) {
    float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }

private:
  float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
  float a1_ = 0.0f, a2_ = 0.0f;
  float z1_ = 0.0f, z2_ = 0.0f;
};

// Double-precision TDF-II biquad for steep low-frequency filters (45-75 Hz @ 48 kHz)
// where poles lie close to z = 1 and 32-bit state/coefficient quantization is undesirable.
class Biquad64 {
public:
  void reset();
  void setHPF(double fc, double fs, double Q);

  inline float process(float x) {
    double xd = static_cast<double>(x);
    double y  = b0_ * xd + z1_;
    z1_ = b1_ * xd - a1_ * y + z2_;
    z2_ = b2_ * xd - a2_ * y;
    return static_cast<float>(y);
  }

private:
  double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0;
  double a1_ = 0.0, a2_ = 0.0;
  double z1_ = 0.0, z2_ = 0.0;
};

// First-order DC-blocking high-pass filter: y[n] = x[n] - x[n-1] + R * y[n-1]
class DcBlocker {
public:
  void configure(float fc, float fs);
  void reset();

  inline float process(float x) {
    float y = x - x1_ + r_ * y1_;
    x1_ = x;
    y1_ = y;
    return y;
  }

private:
  float r_  = 0.998f;
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};

// Fixed-capacity power-of-two sample delay line (zero heap allocation).
template <size_t CAPACITY>
class DelayLine {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

public:
  void reset() {
    for (size_t i = 0; i < CAPACITY; i++) buf_[i] = 0.0f;
    writeIdx_ = 0;
  }

  void setDelaySamples(size_t samples) {
    delay_ = (samples < CAPACITY) ? samples : (CAPACITY - 1);
  }

  inline float process(float x) {
    buf_[writeIdx_] = x;
    size_t readIdx = (writeIdx_ - delay_) & (CAPACITY - 1);
    writeIdx_ = (writeIdx_ + 1) & (CAPACITY - 1);
    return buf_[readIdx];
  }

private:
  float  buf_[CAPACITY] = {};
  size_t writeIdx_      = 0;
  size_t delay_         = 0;
};

// Mid/Side Stereo Spatial Diffuser (recovers L-R stereo ambience & creates Harman CSF room feel):
//   - High-passes S = 0.5*(L-R) above 400 Hz so bass stays 100% tight & mono
//   - 2 prime-length Schroeder all-pass stages (3.7 ms & 6.6 ms, flat unity magnitude)
//   - Warm LPF-damped early reflection tap (~13.8 ms @ 4.8 kHz) for natural 3D envelopment
class SpatialDiffuser {
public:
  void configure(float fs);
  void reset();
  float processSide(float side);

private:
  float  hpAlpha_ = 0.94f;
  float  hpPrevX_ = 0.0f;
  float  hpPrevY_ = 0.0f;

  float  lpAlpha_ = 0.45f;
  float  erLp_    = 0.0f;

  float  ap1Buf_[256]  = {};
  size_t ap1W_         = 0;
  size_t ap1Len_       = 179;

  float  ap2Buf_[512]  = {};
  size_t ap2W_         = 0;
  size_t ap2Len_       = 317;

  float  erBuf_[1024]  = {};
  size_t erW_          = 0;
  size_t erLen_        = 663;
};

// Feed-forward dual-detector soft-knee DRC (fast look-ahead peak + slow RMS thermal)
class BandLimiter {
public:
  void configure(float fs,
                 float lookaheadMs,
                 float peakAtkMs, float peakRelMs, float peakRatio,
                 float rmsAtkMs,  float rmsRelMs,  float rmsRatio,
                 float kneeDb);
  void setThresholds(float peakThrDb, float rmsThrDb);
  void reset();

  float process(float x);

  static float softKneeGainDb(float levelDb, float thrDb, float ratio, float kneeDb);

private:
  DelayLine<128> lookahead_;

  float peakAtkAlpha_ = 0.05f;
  float peakRelAlpha_ = 0.0002f;
  float rmsAtkAlpha_  = 0.0001f;
  float rmsRelAlpha_  = 0.00002f;

  float peakThrDb_    = -1.5f;
  float peakKneeLin_  = 0.5f;
  float peakRatio_    = 20.0f;

  float rmsThrDb_     = -5.5f;
  float rmsKneeSqLin_ = 0.09f;
  float rmsRatio_     = 6.0f;

  float kneeDb_       = 6.0f;

  float peakEnv_      = 0.0f;
  float rmsSqEnv_     = 0.0f;
};

enum class DspMode : uint8_t {
  TUNED         = 0, // Full EQ + adaptive loudness / SysID fit + crossover + DRC
  FLAT_BASELINE = 1  // Part D capture mode: mandatory 48 Hz HPF + LR4 crossover only, unity gain
};

// Full Harman-style 2-way active DSP pipeline for Onyx Studio 5/6:
//   Mono sum -> DC blocker -> Smooth perceptual volume
//   -> Adaptive Loudness (ISO 226 low/high shelves) OR fitted SysidEq cascade (eq_coeffs.h)
//   -> 3-band Global Voicing PEQ
//   -> LR4 Active Crossover
//   -> Woofer: 4th-order dynamic sliding subsonic HPF (Biquad64) + Look-ahead Peak/RMS DRC
//   -> Tweeter: Acoustic time-alignment delay + Look-ahead Peak/RMS DRC
//   -> C1-smooth soft-clipper safety net -> 32-bit stereo I2S frames (OUT1=woofer, OUT2=tweeter)
class AudioCrossover {
public:
  void configure(uint32_t sampleRate);

  // Thread-safe setters (called from AVRCP callback / main loop)
  void    setVolume(uint8_t avrcpVol);
  uint8_t volume() const { return volume_; }

  void setAcPower(bool acPower);
  bool isAcPower() const { return acPower_; }

  void    setMode(DspMode mode) { mode_ = mode; }
  DspMode mode() const { return mode_; }

  // Processes interleaved 16-bit stereo PCM into 32-bit stereo I2S frames (OUT1=woofer, OUT2=tweeter)
  void processBlock(const int16_t *stereoIn, int32_t *i2sOut, size_t frames);

  // Processes mono float32 samples (e.g., 48 kHz SysID Farina sweep) through the active DSP mode
  void processMonoFloatBlock(const float *monoIn, int32_t *i2sOut, size_t frames);

private:
  void updateDynamicControls(size_t frames);
  void applySubsonicHpf(float fcHz);
  void applyLoudnessShelves(float amount);
  void applyRailThresholds(bool acPower);

  // C1-continuous soft-clipper safety net (100% linear up to +-0.94, bounded within +-0.985)
  static inline float softLimitSafety(float x) {
    constexpr float THR  = 0.94f;
    constexpr float HEAD = 0.045f; // Asymptote at 0.94 + 0.045 = 0.985
    float ax = (x >= 0.0f) ? x : -x;
    if (ax <= THR) return x;
    float u = ax - THR;
    float y = THR + (HEAD * u) / (HEAD + u);
    return (x >= 0.0f) ? y : -y;
  }

  float fs_ = 44100.0f;

  // Stage 1: DC blocker + Mid/Side stereo spatial diffuser
  DcBlocker       dcBlock_;
  SpatialDiffuser spatial_;

  // Stage 2: Adaptive Loudness (ISO 226)
  Biquad loudLowShelf_;
  Biquad loudHighShelf_;

  // Stage 3: Global Harman voicing PEQ (fallback) + fitted SysID biquad cascade (eq_coeffs.h)
  Biquad peq1_;
  Biquad peq2_;
  Biquad peq3_;

  static constexpr size_t MAX_SYSID_SECTIONS = 12;
  Biquad sysidEq_[MAX_SYSID_SECTIONS];
  size_t sysidEqCount_ = 0;

  // Stage 4: LR4 2-way active crossover + tweeter acoustic alignment
  Biquad        wfLp1_;
  Biquad        wfLp2_;
  Biquad        twHp1_;
  Biquad        twHp2_;
  DelayLine<32> twDelay_;

  // Stage 5: Woofer 4th-order Butterworth dynamic sliding subsonic HPF (double precision)
  Biquad64 wfSub1_;
  Biquad64 wfSub2_;
  // Fixed mandatory 48 Hz 4th-order Butterworth HPF used in FLAT_BASELINE mode (Part D Stage 1)
  Biquad64 flatHpf1_;
  Biquad64 flatHpf2_;

  // Feed-forward low-bass excursion detector (< 90 Hz tap before sliding HPF)
  float bassTapAlpha_ = 0.012f;
  float bassTapLp_    = 0.0f;
  float excurAtk_     = 0.0015f;
  float excurRel_     = 0.0001f;
  float excurEnv_     = 0.0f;

  // Broadband program level tracker (enables dynamic loudness on both AVRCP & Aux/non-AbsVol BT)
  float progAtk_        = 0.0003f;
  float progRel_        = 0.00002f;
  float progEnv_        = 0.0f;

  // Studio 9 extracted dynamic bass stage (Part D Stage 4: ~92 Hz complementary split)
  float dynBassAlpha_   = 0.012f;
  float dynBassLp_      = 0.0f;
  float dynBassAtk_     = 0.0003f;
  float dynBassRel_     = 0.00002f;
  float dynBassEnv_     = 0.0f;
  float dynBassThrDb_   = -9.5f;
  float dynBassRatio_   = 2.5f;
  float dynBassKneeDb_  = 10.0f;
  float dynBassKneeLin_ = 0.18f;

  // Stage 6: 2-band Look-Ahead Peak + RMS Thermal DRC
  BandLimiter wfLimiter_;
  BandLimiter twLimiter_;

  // Thread-safe control targets (written by AVRCP / main loop, read by audio task)
  volatile uint8_t volume_     = 80;
  volatile float   targetGain_ = 0.34f;
  volatile bool    acPower_    = false; // Fails safe to 10 V battery limits until DC_DET confirms 19 V
  volatile DspMode mode_       = DspMode::TUNED;

  // Audio-thread smoothed runtime state (never mutated outside configure / processBlock)
  float currentGain_      = 0.0f;
  float gainSlewAlpha_    = 0.0015f;
  float currentLoudness_  = -1.0f;
  float appliedLoudness_  = -1.0f;
  float currentSubHz_     = 48.0f;
  float appliedSubHz_     = -1.0f;
  bool  appliedAcPower_   = false;
};
