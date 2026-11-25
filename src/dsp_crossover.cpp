#include "dsp_crossover.h"
#include "board_config.h"
#include "eq_coeffs.h"
#include <math.h>

using namespace BoardConfig;

namespace {

constexpr float kPi       = 3.14159265358979323846f;
constexpr double kPi64    = 3.14159265358979323846;
constexpr float kButterQ  = 0.70710678118654752440f;

// 4th-order Butterworth pole Q factors: Q_k = 1 / (2 * cos((2k-1) * pi / 8))
// Placed Q1 first (damped) then Q2 (peaked) to keep intermediate signal level bounded.
constexpr double kBw4Q1   = 0.54119610014619698440;
constexpr double kBw4Q2   = 1.30656296487637652786;

constexpr float kLn10Over20 = 0.11512925464970228420f;
constexpr float k20OverLn10 = 8.68588963806503655302f;
constexpr float k10OverLn10 = 4.34294481903251827651f;

inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

inline float dbToLin(float db) {
  return expf(db * kLn10Over20);
}

inline float linToDb(float lin) {
  return k20OverLn10 * logf(lin);
}

inline float onePoleAlphaMs(float tauMs, float fs) {
  if (tauMs <= 0.0f || fs <= 0.0f) return 1.0f;
  return 1.0f - expf(-1000.0f / (tauMs * fs));
}

} // namespace

// ============================================================================
// Biquad (32-bit float TDF-II, RBJ Audio EQ Cookbook)
// ============================================================================

void Biquad::reset() {
  z1_ = 0.0f;
  z2_ = 0.0f;
}

void Biquad::setBypass() {
  b0_ = 1.0f; b1_ = 0.0f; b2_ = 0.0f;
  a1_ = 0.0f; a2_ = 0.0f;
}

void Biquad::setCoeffs(float b0, float b1, float b2, float a1, float a2) {
  b0_ = b0; b1_ = b1; b2_ = b2;
  a1_ = a1; a2_ = a2;
}

void Biquad::setLPF(float fc, float fs, float Q) {
  float w0    = 2.0f * kPi * fc / fs;
  float c     = cosf(w0);
  float s     = sinf(w0);
  float alpha = s / (2.0f * Q);
  float a0    = 1.0f + alpha;

  b0_ = ((1.0f - c) * 0.5f) / a0;
  b1_ = (1.0f - c) / a0;
  b2_ = b0_;
  a1_ = (-2.0f * c) / a0;
  a2_ = (1.0f - alpha) / a0;
}

void Biquad::setHPF(float fc, float fs, float Q) {
  float w0    = 2.0f * kPi * fc / fs;
  float c     = cosf(w0);
  float s     = sinf(w0);
  float alpha = s / (2.0f * Q);
  float a0    = 1.0f + alpha;

  b0_ = ((1.0f + c) * 0.5f) / a0;
  b1_ = -(1.0f + c) / a0;
  b2_ = b0_;
  a1_ = (-2.0f * c) / a0;
  a2_ = (1.0f - alpha) / a0;
}

void Biquad::setButterworthLPF(float fc, float fs) {
  setLPF(fc, fs, kButterQ);
}

void Biquad::setButterworthHPF(float fc, float fs) {
  setHPF(fc, fs, kButterQ);
}

void Biquad::setPeakingEQ(float fc, float fs, float Q, float gainDb) {
  if (fabsf(gainDb) < 0.05f) {
    setBypass();
    return;
  }
  float A     = expf(gainDb * (kLn10Over20 * 0.5f)); // 10^(gainDb/40)
  float w0    = 2.0f * kPi * fc / fs;
  float c     = cosf(w0);
  float s     = sinf(w0);
  float alpha = s / (2.0f * Q);
  float a0    = 1.0f + (alpha / A);

  b0_ = (1.0f + alpha * A) / a0;
  b1_ = (-2.0f * c) / a0;
  b2_ = (1.0f - alpha * A) / a0;
  a1_ = (-2.0f * c) / a0;
  a2_ = (1.0f - (alpha / A)) / a0;
}

void Biquad::setLowShelf(float fc, float fs, float slope, float gainDb) {
  if (fabsf(gainDb) < 0.05f) {
    setBypass();
    return;
  }
  float A      = expf(gainDb * (kLn10Over20 * 0.5f)); // 10^(gainDb/40)
  float w0     = 2.0f * kPi * fc / fs;
  float c      = cosf(w0);
  float s      = sinf(w0);
  float term   = (A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f;
  if (term < 0.0f) term = 0.0f;
  float alpha  = (s * 0.5f) * sqrtf(term);
  float beta   = 2.0f * sqrtf(A) * alpha;

  float ap1    = A + 1.0f;
  float am1    = A - 1.0f;
  float a0     = ap1 + am1 * c + beta;

  b0_ = (A * (ap1 - am1 * c + beta)) / a0;
  b1_ = (2.0f * A * (am1 - ap1 * c)) / a0;
  b2_ = (A * (ap1 - am1 * c - beta)) / a0;
  a1_ = (-2.0f * (am1 + ap1 * c)) / a0;
  a2_ = (ap1 + am1 * c - beta) / a0;
}

void Biquad::setHighShelf(float fc, float fs, float slope, float gainDb) {
  if (fabsf(gainDb) < 0.05f) {
    setBypass();
    return;
  }
  float A      = expf(gainDb * (kLn10Over20 * 0.5f)); // 10^(gainDb/40)
  float w0     = 2.0f * kPi * fc / fs;
  float c      = cosf(w0);
  float s      = sinf(w0);
  float term   = (A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f;
  if (term < 0.0f) term = 0.0f;
  float alpha  = (s * 0.5f) * sqrtf(term);
  float beta   = 2.0f * sqrtf(A) * alpha;

  float ap1    = A + 1.0f;
  float am1    = A - 1.0f;
  float a0     = ap1 - am1 * c + beta;

  b0_ = (A * (ap1 + am1 * c + beta)) / a0;
  b1_ = (-2.0f * A * (am1 + ap1 * c)) / a0;
  b2_ = (A * (ap1 + am1 * c - beta)) / a0;
  a1_ = (2.0f * (am1 - ap1 * c)) / a0;
  a2_ = (ap1 - am1 * c - beta) / a0;
}

void Biquad::setAllPass(float fc, float fs, float Q) {
  float w0    = 2.0f * kPi * fc / fs;
  float c     = cosf(w0);
  float s     = sinf(w0);
  float alpha = s / (2.0f * Q);
  float a0    = 1.0f + alpha;

  b0_ = (1.0f - alpha) / a0;
  b1_ = (-2.0f * c) / a0;
  b2_ = (1.0f + alpha) / a0;
  a1_ = (-2.0f * c) / a0;
  a2_ = (1.0f - alpha) / a0;
}

// ============================================================================
// Biquad64 (64-bit double TDF-II for low-frequency subsonic HPF precision)
// ============================================================================

void Biquad64::reset() {
  z1_ = 0.0;
  z2_ = 0.0;
}

void Biquad64::setHPF(double fc, double fs, double Q) {
  double w0    = 2.0 * kPi64 * fc / fs;
  double c     = cos(w0);
  double s     = sin(w0);
  double alpha = s / (2.0 * Q);
  double a0    = 1.0 + alpha;

  b0_ = ((1.0 + c) * 0.5) / a0;
  b1_ = -(1.0 + c) / a0;
  b2_ = b0_;
  a1_ = (-2.0 * c) / a0;
  a2_ = (1.0 - alpha) / a0;
}

// ============================================================================
// DcBlocker
// ============================================================================

void DcBlocker::configure(float fc, float fs) {
  r_ = expf(-2.0f * kPi * fc / fs);
  reset();
}

void DcBlocker::reset() {
  x1_ = 0.0f;
  y1_ = 0.0f;
}

// ============================================================================
// SpatialDiffuser (Mid/Side Stereo Ambience Recovery — Harman CSF Room Feel)
// ============================================================================

void SpatialDiffuser::configure(float fs) {
  hpAlpha_ = expf(-2.0f * kPi * 400.0f / fs);
  lpAlpha_ = 1.0f - expf(-2.0f * kPi * 4800.0f / fs);

  auto clampLen = [](float sec, float sr, size_t cap) -> size_t {
    size_t n = static_cast<size_t>(lroundf(sec * sr));
    if (n < 1) n = 1;
    if (n >= cap) n = cap - 1;
    return n;
  };
  ap1Len_ = clampLen(0.00373f, fs, 256);  // ~3.73 ms prime diffuser
  ap2Len_ = clampLen(0.00661f, fs, 512);  // ~6.61 ms prime diffuser
  erLen_  = clampLen(0.01381f, fs, 1024); // ~13.81 ms warm early reflection
  reset();
}

void SpatialDiffuser::reset() {
  hpPrevX_ = 0.0f;
  hpPrevY_ = 0.0f;
  erLp_    = 0.0f;
  ap1W_    = 0;
  ap2W_    = 0;
  erW_     = 0;
  for (size_t i = 0; i < 256; i++)  ap1Buf_[i] = 0.0f;
  for (size_t i = 0; i < 512; i++)  ap2Buf_[i] = 0.0f;
  for (size_t i = 0; i < 1024; i++) erBuf_[i]  = 0.0f;
}

float SpatialDiffuser::processSide(float side) {
  // 1st-order HPF @ 400 Hz: keeps bass 100% tight & mono
  float hp = hpAlpha_ * (hpPrevY_ + side - hpPrevX_);
  hpPrevX_ = side;
  hpPrevY_ = hp;

  // Schroeder All-Pass 1 (flat unity magnitude, decorrelates stereo ambience)
  constexpr float g1 = 0.50f;
  size_t r1 = (ap1W_ - ap1Len_) & 255;
  float  d1 = ap1Buf_[r1];
  float  v1 = hp + g1 * d1;
  ap1Buf_[ap1W_] = v1;
  ap1W_ = (ap1W_ + 1) & 255;
  float  y1 = d1 - g1 * v1;

  // Schroeder All-Pass 2
  constexpr float g2 = 0.45f;
  size_t r2 = (ap2W_ - ap2Len_) & 511;
  float  d2 = ap2Buf_[r2];
  float  v2 = y1 + g2 * d2;
  ap2Buf_[ap2W_] = v2;
  ap2W_ = (ap2W_ + 1) & 511;
  float  y2 = d2 - g2 * v2;

  // Warm LPF-damped early reflection (~13.8 ms @ 4.8 kHz) for room envelopment
  size_t rEr = (erW_ - erLen_) & 1023;
  float  er  = erBuf_[rEr];
  erLp_ += lpAlpha_ * (y2 - erLp_);
  erBuf_[erW_] = erLp_;
  erW_ = (erW_ + 1) & 1023;

  return 0.28f * y2 + 0.14f * er;
}

// ============================================================================
// BandLimiter (Look-Ahead Fast Peak + Slow RMS Soft-Knee DRC)
// ============================================================================

void BandLimiter::configure(float fs,
                            float lookaheadMs,
                            float peakAtkMs, float peakRelMs, float peakRatio,
                            float rmsAtkMs,  float rmsRelMs,  float rmsRatio,
                            float kneeDb) {
  size_t delaySamp = static_cast<size_t>(lroundf(lookaheadMs * fs * 0.001f));
  lookahead_.setDelaySamples(delaySamp);

  peakAtkAlpha_ = onePoleAlphaMs(peakAtkMs, fs);
  peakRelAlpha_ = onePoleAlphaMs(peakRelMs, fs);
  rmsAtkAlpha_  = onePoleAlphaMs(rmsAtkMs, fs);
  rmsRelAlpha_  = onePoleAlphaMs(rmsRelMs, fs);

  peakRatio_ = (peakRatio < 1.0f) ? 1.0f : peakRatio;
  rmsRatio_  = (rmsRatio  < 1.0f) ? 1.0f : rmsRatio;
  kneeDb_    = (kneeDb    < 0.1f) ? 0.1f : kneeDb;

  reset();
}

void BandLimiter::setThresholds(float peakThrDb, float rmsThrDb) {
  peakThrDb_    = peakThrDb;
  rmsThrDb_     = rmsThrDb;
  peakKneeLin_  = dbToLin(peakThrDb_ - 0.5f * kneeDb_);
  float rmsKnee = dbToLin(rmsThrDb_  - 0.5f * kneeDb_);
  rmsKneeSqLin_ = rmsKnee * rmsKnee;
}

void BandLimiter::reset() {
  lookahead_.reset();
  peakEnv_  = 0.0f;
  rmsSqEnv_ = 0.0f;
}

float BandLimiter::softKneeGainDb(float levelDb, float thrDb, float ratio, float kneeDb) {
  float over  = levelDb - thrDb;
  float halfW = 0.5f * kneeDb;
  if (over <= -halfW) return 0.0f;

  float slope = (1.0f / ratio) - 1.0f; // <= 0
  if (over < halfW) {
    float xk = over + halfW;
    return slope * (xk * xk) / (2.0f * kneeDb);
  }
  return slope * over;
}

float BandLimiter::process(float x) {
  float ax      = (x >= 0.0f) ? x : -x;
  float delayed = lookahead_.process(x);

  // Fast peak envelope (asymmetric attack / release)
  float pAlpha = (ax > peakEnv_) ? peakAtkAlpha_ : peakRelAlpha_;
  peakEnv_ += pAlpha * (ax - peakEnv_);

  // Slow RMS power envelope (thermal & PSU protection)
  float xSq    = x * x;
  float rAlpha = (xSq > rmsSqEnv_) ? rmsAtkAlpha_ : rmsRelAlpha_;
  rmsSqEnv_ += rAlpha * (xSq - rmsSqEnv_);

  float gainDb = 0.0f;
  if (peakEnv_ > peakKneeLin_) {
    float peakDb = linToDb(peakEnv_);
    float gPeak  = softKneeGainDb(peakDb, peakThrDb_, peakRatio_, kneeDb_);
    if (gPeak < gainDb) gainDb = gPeak;
  }

  if (rmsSqEnv_ > rmsKneeSqLin_) {
    // 10 * log10(rmsSq) == 20 * log10(sqrt(rmsSq)) without calling sqrtf
    float rmsDb = k10OverLn10 * logf(rmsSqEnv_);
    float gRms  = softKneeGainDb(rmsDb, rmsThrDb_, rmsRatio_, kneeDb_);
    if (gRms < gainDb) gainDb = gRms;
  }

  if (gainDb >= -1e-4f) return delayed;
  return delayed * dbToLin(gainDb);
}

// ============================================================================
// AudioCrossover (Full Harman-Style 2-Way Active DSP Pipeline)
// ============================================================================

void AudioCrossover::configure(uint32_t sampleRate) {
  fs_ = static_cast<float>(sampleRate);

  // Stage 1: DC blocker + Mid/Side spatial diffuser + butter-smooth volume slewing (~45 ms)
  dcBlock_.configure(DC_BLOCK_HZ, fs_);
  spatial_.configure(fs_);
  gainSlewAlpha_ = onePoleAlphaMs(45.0f, fs_);
  currentGain_   = targetGain_;

  // Stage 3: Global Harman voicing PEQ (fallback) + fitted SysID biquad cascade (eq_coeffs.h)
  peq1_.setPeakingEQ(PEQ1_HZ, fs_, PEQ1_Q, PEQ1_DB);
  peq2_.setPeakingEQ(PEQ2_HZ, fs_, PEQ2_Q, PEQ2_DB);
  peq3_.setPeakingEQ(PEQ3_HZ, fs_, PEQ3_Q, PEQ3_DB);
  peq1_.reset();
  peq2_.reset();
  peq3_.reset();

  sysidEqCount_ = 0;
  if (SysidEq::EQ_HAS_FIT) {
    sysidEqCount_ = (SysidEq::EQ_NUM_SECTIONS <= MAX_SYSID_SECTIONS)
                        ? SysidEq::EQ_NUM_SECTIONS
                        : MAX_SYSID_SECTIONS;
    for (size_t k = 0; k < sysidEqCount_; k++) {
      const auto &sec = SysidEq::EQ_SECTIONS[k];
      if (sampleRate == SysidEq::EQ_REF_SAMPLE_RATE) {
        sysidEq_[k].setCoeffs(sec.b0, sec.b1, sec.b2, sec.a1, sec.a2);
      } else {
        sysidEq_[k].setPeakingEQ(sec.f0, fs_, sec.Q, sec.gainDb);
      }
      sysidEq_[k].reset();
    }
  }

  // Mandatory fixed 48 Hz 4th-order Butterworth HPF for FLAT_BASELINE & SysID fitted mode
  flatHpf1_.setHPF(static_cast<double>(SUBSONIC_HPF_HZ), static_cast<double>(fs_), kBw4Q1);
  flatHpf2_.setHPF(static_cast<double>(SUBSONIC_HPF_HZ), static_cast<double>(fs_), kBw4Q2);
  flatHpf1_.reset();
  flatHpf2_.reset();

  // Stage 4: LR4 2-way active crossover + tweeter acoustic delay
  wfLp1_.setButterworthLPF(CROSSOVER_HZ, fs_);
  wfLp2_.setButterworthLPF(CROSSOVER_HZ, fs_);
  twHp1_.setButterworthHPF(CROSSOVER_HZ, fs_);
  twHp2_.setButterworthHPF(CROSSOVER_HZ, fs_);
  wfLp1_.reset();
  wfLp2_.reset();
  twHp1_.reset();
  twHp2_.reset();

  size_t twDelaySamp = static_cast<size_t>(lroundf(TWEETER_DELAY_US * fs_ * 1e-6f));
  twDelay_.setDelaySamples(twDelaySamp);
  twDelay_.reset();

  // Stage 5: Feed-forward bass excursion tap (~90 Hz 1-pole LPF) + 4th-order subsonic HPF
  bassTapAlpha_ = 1.0f - expf(-2.0f * kPi * 90.0f / fs_);
  bassTapLp_    = 0.0f;
  excurAtk_     = onePoleAlphaMs(15.0f, fs_);
  excurRel_     = onePoleAlphaMs(250.0f, fs_);
  excurEnv_     = 0.0f;

  // Broadband program level tracker for volume- + signal-aware dynamic loudness (BT & Aux)
  progAtk_      = onePoleAlphaMs(80.0f, fs_);
  progRel_      = onePoleAlphaMs(1200.0f, fs_);
  progEnv_      = 0.0f;

  // Studio 9 extracted dynamic bass stage (Part D Stage 4: ~92 Hz complementary split)
  float drcFc    = (SysidEq::DRC_CONFIG.sidechainHz > 30.0f) ? SysidEq::DRC_CONFIG.sidechainHz : 92.0f;
  float drcAtk   = (SysidEq::DRC_CONFIG.attackMs > 1.0f)     ? SysidEq::DRC_CONFIG.attackMs    : 45.0f;
  float drcRel   = (SysidEq::DRC_CONFIG.releaseMs > 20.0f)   ? SysidEq::DRC_CONFIG.releaseMs   : 400.0f;
  dynBassRatio_  = (SysidEq::DRC_CONFIG.ratio >= 1.05f)      ? SysidEq::DRC_CONFIG.ratio       : 2.5f;
  dynBassKneeDb_ = (SysidEq::DRC_CONFIG.kneeDb >= 0.5f)      ? SysidEq::DRC_CONFIG.kneeDb      : 10.0f;
  dynBassAlpha_  = 1.0f - expf(-2.0f * kPi * drcFc / fs_);
  dynBassLp_     = 0.0f;
  dynBassAtk_    = onePoleAlphaMs(drcAtk, fs_);
  dynBassRel_    = onePoleAlphaMs(drcRel, fs_);
  dynBassEnv_    = 0.0f;

  // Stage 6: 2-band Look-Ahead Peak + RMS Thermal Limiters
  wfLimiter_.configure(fs_, DRC_LOOKAHEAD_MS,
                       WF_PEAK_ATK_MS, WF_PEAK_REL_MS, DRC_PEAK_RATIO,
                       WF_RMS_ATK_MS,  WF_RMS_REL_MS,  DRC_RMS_RATIO,
                       DRC_KNEE_DB);
  twLimiter_.configure(fs_, DRC_LOOKAHEAD_MS,
                       TW_PEAK_ATK_MS, TW_PEAK_REL_MS, DRC_PEAK_RATIO,
                       TW_RMS_ATK_MS,  TW_RMS_REL_MS,  DRC_RMS_RATIO,
                       DRC_KNEE_DB);

  // Initialize dynamic shelves, subsonic HPF, and rail thresholds immediately
  bool ac = acPower_;
  appliedAcPower_ = ac;
  applyRailThresholds(ac);

  currentSubHz_ = ac ? SUBSONIC_AC_MIN_HZ : SUBSONIC_BAT_MIN_HZ;
  appliedSubHz_ = -1.0f;
  applySubsonicHpf(currentSubHz_);
  wfSub1_.reset();
  wfSub2_.reset();

  currentLoudness_ = -1.0f;
  appliedLoudness_ = -1.0f;
  updateDynamicControls(1);
  loudLowShelf_.reset();
  loudHighShelf_.reset();
}

void AudioCrossover::setVolume(uint8_t avrcpVol) {
  if (avrcpVol > 127) avrcpVol = 127;
  volume_ = avrcpVol;
  if (avrcpVol == 0) {
    targetGain_ = 0.0f;
  } else {
    // Gentle Harman perceptual taper (norm^1.35): matches Studio 9 loudness at 38% phone volume
    // and keeps every phone volume click smooth (~1.5..2.2 dB per notch, zero abrupt jumps)
    float norm  = static_cast<float>(avrcpVol) / 127.0f;
    targetGain_ = MASTER_GAIN * powf(norm, 1.35f);
  }
}

void AudioCrossover::setAcPower(bool acPower) {
  acPower_ = acPower;
}

void AudioCrossover::applyRailThresholds(bool acPower) {
  float baseThr = SysidEq::DRC_CONFIG.thresholdDbfs;
  if (acPower) {
    wfLimiter_.setThresholds(WF_PEAK_THR_AC_DB, WF_RMS_THR_AC_DB);
    twLimiter_.setThresholds(TW_PEAK_THR_AC_DB, TW_RMS_THR_AC_DB);
    dynBassThrDb_ = baseThr;        // 19 V AC: Studio 9 dynamic bass threshold from eq_coeffs.h
  } else {
    wfLimiter_.setThresholds(WF_PEAK_THR_BAT_DB, WF_RMS_THR_BAT_DB);
    twLimiter_.setThresholds(TW_PEAK_THR_BAT_DB, TW_RMS_THR_BAT_DB);
    dynBassThrDb_ = baseThr - 3.0f; // 10 V Battery: engages 3 dB earlier to guard boost rail
  }
  dynBassKneeLin_ = dbToLin(dynBassThrDb_ - 0.5f * dynBassKneeDb_);
}

void AudioCrossover::applySubsonicHpf(float fcHz) {
  double fc = static_cast<double>(fcHz);
  double fs = static_cast<double>(fs_);
  wfSub1_.setHPF(fc, fs, kBw4Q1);
  wfSub2_.setHPF(fc, fs, kBw4Q2);
  appliedSubHz_ = fcHz;
}

void AudioCrossover::applyLoudnessShelves(float amount) {
  loudLowShelf_.setLowShelf(LOUDNESS_LOW_HZ, fs_, LOUDNESS_SHELF_SLOPE,
                            LOUDNESS_LOW_MAX_DB * amount);
  loudHighShelf_.setHighShelf(LOUDNESS_HIGH_HZ, fs_, LOUDNESS_SHELF_SLOPE,
                              LOUDNESS_HIGH_MAX_DB * amount);
  appliedLoudness_ = amount;
}

void AudioCrossover::updateDynamicControls(size_t frames) {
  bool    ac  = acPower_;
  uint8_t vol = volume_;

  if (ac != appliedAcPower_) {
    appliedAcPower_ = ac;
    applyRailThresholds(ac);
  }

  float blockMs = (1000.0f * static_cast<float>(frames)) / fs_;

  // 1. Adaptive Loudness amount [0.0 .. 1.0]: full deep bass at low volume, flat at high volume.
  // Combines AVRCP volume with actual post-volume signal envelope so dynamic bass works equally
  // well on Bluetooth (Absolute Volume ON or OFF) and Aux-In.
  float flatVol = static_cast<float>(ac ? LOUDNESS_VOL_FLAT_AC : LOUDNESS_VOL_FLAT_BAT);
  float fullVol = static_cast<float>(LOUDNESS_VOL_FULL);
  float volLoud = 1.0f - clampf((static_cast<float>(vol) - fullVol) / (flatVol - fullVol),
                                0.0f, 1.0f);
  float sigDb   = (progEnv_ > 1e-4f) ? linToDb(progEnv_) : -80.0f;
  float sigLoud = 1.0f - clampf((sigDb - (-32.0f)) / ((-12.0f) - (-32.0f)), 0.0f, 1.0f);
  float maxLoud = (vol >= 105) ? 0.35f : 1.0f;
  float targetLoud = (volLoud > sigLoud) ? volLoud : sigLoud;
  if (targetLoud > maxLoud) targetLoud = maxLoud;

  if (currentLoudness_ < 0.0f) {
    currentLoudness_ = targetLoud;
  } else {
    float slew = 1.0f - expf(-blockMs / 50.0f); // 50 ms smooth loudness transition
    currentLoudness_ += slew * (targetLoud - currentLoudness_);
  }

  if (fabsf(currentLoudness_ - appliedLoudness_) > 0.01f) {
    applyLoudnessShelves(currentLoudness_);
  }

  // 2. Dynamic Sliding Subsonic HPF (passive-radiator excursion & battery rail protection)
  float minHz = ac ? SUBSONIC_AC_MIN_HZ : SUBSONIC_BAT_MIN_HZ;
  float maxHz = ac ? SUBSONIC_AC_MAX_HZ : SUBSONIC_BAT_MAX_HZ;

  // Volume pre-lift on battery near max output + real-time feed-forward low-bass excursion lift
  float volLift   = ac ? 0.0f : (0.35f * clampf((static_cast<float>(vol) - 90.0f) / 37.0f, 0.0f, 1.0f));
  float excurLift = clampf((excurEnv_ - SUBSONIC_EXCUR_THR) / (0.95f - SUBSONIC_EXCUR_THR), 0.0f, 1.0f);
  float slide     = (excurLift > volLift) ? excurLift : volLift;
  float targetHz  = minHz + slide * (maxHz - minHz);

  float tauSubMs  = (targetHz > currentSubHz_) ? 35.0f : 280.0f; // Fast attack, gentle release
  float subSlew   = 1.0f - expf(-blockMs / tauSubMs);
  currentSubHz_  += subSlew * (targetHz - currentSubHz_);

  if (fabsf(currentSubHz_ - appliedSubHz_) > 0.25f) {
    applySubsonicHpf(currentSubHz_);
  }
}

void AudioCrossover::processBlock(const int16_t *stereoIn, int32_t *i2sOut, size_t frames) {
  if (frames == 0) return;

  constexpr float kInv32768 = 1.0f / 32768.0f;

  // FLAT_BASELINE mode (Part D): mandatory 48 Hz HPF + LR4 crossover only, unity gain
  if (mode_ == DspMode::FLAT_BASELINE) {
    for (size_t i = 0; i < frames; i++) {
      float mono = 0.5f * (static_cast<float>(stereoIn[2 * i]) +
                           static_cast<float>(stereoIn[2 * i + 1])) * kInv32768;
      float prot   = flatHpf2_.process(flatHpf1_.process(mono));
      float wfBand = wfLp2_.process(wfLp1_.process(prot));
      float twBand = twHp2_.process(twHp1_.process(prot));

      int32_t wf32 = static_cast<int32_t>(softLimitSafety(wfBand) * 2147483647.0f);
      int32_t tw32 = static_cast<int32_t>(softLimitSafety(twBand) * 2147483647.0f);
      if (SWAP_WOOFER_TWEETER) {
        i2sOut[2 * i]     = tw32;
        i2sOut[2 * i + 1] = wf32;
      } else {
        i2sOut[2 * i]     = wf32;
        i2sOut[2 * i + 1] = tw32;
      }
    }
    return;
  }

  // Update dynamic filter coefficients & AC/battery ceilings safely on the audio thread
  updateDynamicControls(frames);

  const float tgtGain = targetGain_;

  for (size_t i = 0; i < frames; i++) {
    // Smooth volume gain per sample (zipper-free AVRCP volume changes)
    currentGain_ += gainSlewAlpha_ * (tgtGain - currentGain_);

    // Stage 1: Mid/Side stereo spatial diffusion + DC offset blocker + smooth perceptual volume
    //   Mid  = 0.5*(L+R) stays pure & punchy (vocals/kick/bass centered with 0 ms latency)
    //   Side = 0.5*(L-R) (> 400 Hz) is phase-diffused & softly reflected for Harman CSF room feel
    float chL  = static_cast<float>(stereoIn[2 * i])     * kInv32768;
    float chR  = static_cast<float>(stereoIn[2 * i + 1]) * kInv32768;
    float mid  = 0.5f * (chL + chR);
    float side = 0.5f * (chL - chR);
    float mono = (dcBlock_.process(mid) + spatial_.processSide(side)) * currentGain_;

    // Track actual post-volume program envelope for signal-aware dynamic loudness
    float monoAbs = (mono >= 0.0f) ? mono : -mono;
    float pAlpha  = (monoAbs > progEnv_) ? progAtk_ : progRel_;
    progEnv_     += pAlpha * (monoAbs - progEnv_);

    float wfBand = 0.0f;
    float twBand = 0.0f;

    if (SysidEq::EQ_HAS_FIT && sysidEqCount_ > 0) {
      // Fitted Studio 9 acoustic target chain with Dynamic Loudness EQ & Sliding Subsonic HPF:
      //   Dynamic Loudness shelves (up to +8 dB < 88 Hz at low vol, 0 dB at max vol)
      //   -> pre-gain -> fitted 8-biquad Studio 9 EQ -> Studio 9 92 Hz Dynamic Bass DRC
      //   -> LR4 crossover -> Woofer dynamic sliding subsonic HPF (40..70 Hz) -> Limiters
      float x = loudHighShelf_.process(loudLowShelf_.process(mono)) * SysidEq::EQ_PRE_GAIN_LIN;
      for (size_t k = 0; k < sysidEqCount_; k++) {
        x = sysidEq_[k].process(x);
      }

      if (SysidEq::DRC_CONFIG.enabled) {
        // Complementary 1-pole low-band split (~92 Hz): sums identically to x when GR = 0 dB
        dynBassLp_ += dynBassAlpha_ * (x - dynBassLp_);
        float low    = dynBassLp_;
        float rest   = x - low;
        float lowAbs = (low >= 0.0f) ? low : -low;
        float dAlpha = (lowAbs > dynBassEnv_) ? dynBassAtk_ : dynBassRel_;
        dynBassEnv_ += dAlpha * (lowAbs - dynBassEnv_);
        if (dynBassEnv_ > dynBassKneeLin_) {
          float lvlDb = linToDb(dynBassEnv_);
          float grDb  = BandLimiter::softKneeGainDb(lvlDb, dynBassThrDb_, dynBassRatio_, dynBassKneeDb_);
          if (grDb < -6.5f) grDb = -6.5f;
          low *= dbToLin(grDb);
        }
        x = low + rest;
      }

      // 2.5 kHz LR4 crossover + measured 150 us tweeter voice-coil time alignment
      float wfPreSub = wfLp2_.process(wfLp1_.process(x));
      twBand         = twLimiter_.process(twDelay_.process(twHp2_.process(twHp1_.process(x))));

      // Feed-forward low-bass (< 90 Hz) excursion detector drives sliding subsonic HPF (40..70 Hz)
      bassTapLp_ += bassTapAlpha_ * (wfPreSub - bassTapLp_);
      float bassAbs = (bassTapLp_ >= 0.0f) ? bassTapLp_ : -bassTapLp_;
      float eAlpha  = (bassAbs > excurEnv_) ? excurAtk_ : excurRel_;
      excurEnv_    += eAlpha * (bassAbs - excurEnv_);

      wfBand = wfLimiter_.process(wfSub2_.process(wfSub1_.process(wfPreSub)));
    } else {
      // Stage 2: Adaptive Loudness (ISO 226 volume-dependent low/high shelves)
      mono = loudHighShelf_.process(loudLowShelf_.process(mono));

      // Stage 3: Global Harman voicing PEQ
      mono = peq3_.process(peq2_.process(peq1_.process(mono)));

      // Stage 4: LR4 active crossover split
      float wfPreSub = wfLp2_.process(wfLp1_.process(mono));
      twBand         = twHp2_.process(twHp1_.process(mono));

      // Feed-forward low-bass (< 90 Hz) excursion detector tapped BEFORE sliding HPF
      bassTapLp_ += bassTapAlpha_ * (wfPreSub - bassTapLp_);
      float bassAbs = (bassTapLp_ >= 0.0f) ? bassTapLp_ : -bassTapLp_;
      float eAlpha  = (bassAbs > excurEnv_) ? excurAtk_ : excurRel_;
      excurEnv_    += eAlpha * (bassAbs - excurEnv_);

      // Stage 5: 4th-order Butterworth dynamic sliding subsonic HPF (double precision)
      wfBand = wfSub2_.process(wfSub1_.process(wfPreSub));

      // Tweeter acoustic voice-coil time alignment
      twBand = twDelay_.process(twBand);

      // Stage 6: Independent 2-band look-ahead peak + slow RMS thermal limiters
      wfBand = wfLimiter_.process(wfBand);
      twBand = twLimiter_.process(twBand);
    }

    // Stage 7: C1-smooth safety soft-clipper -> 32-bit I2S stereo frame
    int32_t wf32 = static_cast<int32_t>(softLimitSafety(wfBand) * 2147483647.0f);
    int32_t tw32 = static_cast<int32_t>(softLimitSafety(twBand) * 2147483647.0f);

    if (SWAP_WOOFER_TWEETER) {
      i2sOut[2 * i]     = tw32;
      i2sOut[2 * i + 1] = wf32;
    } else {
      i2sOut[2 * i]     = wf32;
      i2sOut[2 * i + 1] = tw32;
    }
  }
}

void AudioCrossover::processMonoFloatBlock(const float *monoIn, int32_t *i2sOut, size_t frames) {
  if (frames == 0) return;

  if (mode_ == DspMode::FLAT_BASELINE) {
    // Part D Flat Baseline: Stage 1 (48 Hz 4th-order HPF) + Stage 6 (LR4 crossover), 0 dB gain
    for (size_t i = 0; i < frames; i++) {
      float prot   = flatHpf2_.process(flatHpf1_.process(monoIn[i]));
      float wfBand = wfLp2_.process(wfLp1_.process(prot));
      float twBand = twHp2_.process(twHp1_.process(prot));

      int32_t wf32 = static_cast<int32_t>(softLimitSafety(wfBand) * 2147483647.0f);
      int32_t tw32 = static_cast<int32_t>(softLimitSafety(twBand) * 2147483647.0f);
      if (SWAP_WOOFER_TWEETER) {
        i2sOut[2 * i]     = tw32;
        i2sOut[2 * i + 1] = wf32;
      } else {
        i2sOut[2 * i]     = wf32;
        i2sOut[2 * i + 1] = tw32;
      }
    }
    return;
  }

  // Tuned validation sweep (0 dB reference digital gain through active tuned chain)
  updateDynamicControls(frames);
  for (size_t i = 0; i < frames; i++) {
    float mono   = dcBlock_.process(monoIn[i]);
    float wfBand = 0.0f;
    float twBand = 0.0f;

    if (SysidEq::EQ_HAS_FIT && sysidEqCount_ > 0) {
      // Match playback chain (at 0 dB ref gain, without volume-dependent loudness shelf):
      //   pre-gain -> fitted EQ -> 92 Hz Dynamic Bass DRC (inactive at -20 dBFS sweep)
      //   -> LR4 crossover -> tweeter 150 us delay + woofer sliding subsonic HPF -> limiters
      float x = mono * SysidEq::EQ_PRE_GAIN_LIN;
      for (size_t k = 0; k < sysidEqCount_; k++) {
        x = sysidEq_[k].process(x);
      }

      if (SysidEq::DRC_CONFIG.enabled) {
        dynBassLp_ += dynBassAlpha_ * (x - dynBassLp_);
        float low    = dynBassLp_;
        float rest   = x - low;
        float lowAbs = (low >= 0.0f) ? low : -low;
        float dAlpha = (lowAbs > dynBassEnv_) ? dynBassAtk_ : dynBassRel_;
        dynBassEnv_ += dAlpha * (lowAbs - dynBassEnv_);
        if (dynBassEnv_ > dynBassKneeLin_) {
          float lvlDb = linToDb(dynBassEnv_);
          float grDb  = BandLimiter::softKneeGainDb(lvlDb, dynBassThrDb_, dynBassRatio_, dynBassKneeDb_);
          if (grDb < -6.5f) grDb = -6.5f;
          low *= dbToLin(grDb);
        }
        x = low + rest;
      }

      float wfPreSub = wfLp2_.process(wfLp1_.process(x));
      twBand         = twLimiter_.process(twDelay_.process(twHp2_.process(twHp1_.process(x))));

      bassTapLp_ += bassTapAlpha_ * (wfPreSub - bassTapLp_);
      float bassAbs = (bassTapLp_ >= 0.0f) ? bassTapLp_ : -bassTapLp_;
      float eAlpha  = (bassAbs > excurEnv_) ? excurAtk_ : excurRel_;
      excurEnv_    += eAlpha * (bassAbs - excurEnv_);

      wfBand = wfLimiter_.process(wfSub2_.process(wfSub1_.process(wfPreSub)));
    } else {
      mono = loudHighShelf_.process(loudLowShelf_.process(mono));
      mono = peq3_.process(peq2_.process(peq1_.process(mono)));
      float wfPreSub = wfLp2_.process(wfLp1_.process(mono));
      twBand         = twLimiter_.process(twDelay_.process(twHp2_.process(twHp1_.process(mono))));
      wfBand         = wfLimiter_.process(wfSub2_.process(wfSub1_.process(wfPreSub)));
    }

    int32_t wf32 = static_cast<int32_t>(softLimitSafety(wfBand) * 2147483647.0f);
    int32_t tw32 = static_cast<int32_t>(softLimitSafety(twBand) * 2147483647.0f);
    if (SWAP_WOOFER_TWEETER) {
      i2sOut[2 * i]     = tw32;
      i2sOut[2 * i + 1] = wf32;
    } else {
      i2sOut[2 * i]     = wf32;
      i2sOut[2 * i + 1] = tw32;
    }
  }
}
