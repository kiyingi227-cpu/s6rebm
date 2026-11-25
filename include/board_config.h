#pragma once
#include <Arduino.h>

// ============================================================================
// HK Onyx Studio 5 (ESP32 DevKit replacing BM20IK) — Hardware Configuration
// ============================================================================
// Note: Serial is NOT used because TX0/RX0 are wired to WHITE_LED / AMP_PWR_CTRL.
// If firmware upload hangs at "Connecting", unplug RX0 (AMP_PWR_CTRL) or hold BOOT.

namespace BoardConfig {

// ---------------- Pin Map (as built) ----------------
constexpr int PIN_SDA            = 21;
constexpr int PIN_SCL            = 22;

constexpr int PIN_I2S_BCLK       = 25;
constexpr int PIN_I2S_DOUT       = 26;
constexpr int PIN_I2S_LRCLK      = 14;
constexpr int PIN_I2S_DIN        = 27;  // Driven by AK7755 SDOUT3 — MUST stay INPUT

constexpr int PIN_PWR_OFF_READY  = 4;   // To SLG4V4826 pin 5: HIGH = keep speaker latched ON
constexpr int PIN_PWR_ON_OFF     = 17;  // From SLG4V4826 pin 4: pulses HIGH on Power Button press
constexpr int PIN_VREG_EN        = 15;  // From SLG4V4826 pin 11 (TP34, input)
constexpr int PIN_PWR_ONOFF_OP   = 16;  // Unused net (unpopulated circuit)

constexpr int PIN_DSP_PDN        = 5;   // AK7755 PDN: LOW = power-down, HIGH = run
constexpr int PIN_MUTE           = 19;  // To Q17 NPN -> TPA3116 pin 12 (active-HIGH mute)
constexpr int PIN_AMP_PWR        = 3;   // RX0 -> Q2 NPN -> Q18 ME4435 P-FET amp supply switch

constexpr int PIN_RED_LED        = 12;  // Strapping pin: output only, no external pull-up
constexpr int PIN_WHITE_LED      = 1;   // TX0
constexpr int PIN_SHDN           = 2;   // Unused LED driver shutdown (strapping pin, keep INPUT)

// Front buttons
constexpr int PIN_BTN_BLUE       = 32;
constexpr int PIN_BTN_VOL_UP     = 33;
constexpr int PIN_BTN_VOL_DOWN   = 13;
constexpr int PIN_BTN_PLAY_PAUSE = 18;

// Battery, charger, adapter & Aux-In sense
constexpr int PIN_BAT_TEMP_DET   = 36;  // VP (ADC): 3.3V -> 10k -> (4.7k || 10k NTC)
constexpr int PIN_BAT_DET        = 39;  // VN (ADC): 300k / 100k divider -> V_bat = 4.0 * V_adc
constexpr int PIN_DC_DET         = 34;  // 220k / 39k divider -> HIGH (~2.86 V) on 19 V adapter
constexpr int PIN_AUX_IN_DET     = 35;  // Aux jack detect (input-only pin)
constexpr int PIN_CHG_STATUS     = 23;  // bq24090 /CHG (active-LOW open-drain, R50 10k pull-up)

// ---------------- Signal Polarities ----------------
constexpr uint8_t AMP_ON   = HIGH;  // HIGH -> Q2 ON -> Q18 P-FET ON
constexpr uint8_t AMP_OFF  = LOW;
// TPA3116 pin 12 is active-HIGH MUTE pulled up to GVDD by R302.
// Q17 inverts MUTE: LOW -> Q17 OFF -> pin 12 HIGH (MUTED); HIGH -> Q17 ON -> pin 12 LOW (UNMUTED).
constexpr uint8_t MUTE_ON  = LOW;
constexpr uint8_t MUTE_OFF = HIGH;
constexpr uint8_t PDN_RUN  = HIGH;
constexpr uint8_t LED_ON   = HIGH;

// ---------------- LEDC PWM Config ----------------
constexpr uint8_t  LED_CH_WHITE = 0;
constexpr uint8_t  LED_CH_RED   = 1;
constexpr uint32_t LED_PWM_FREQ = 5000;
constexpr uint8_t  LED_PWM_BITS = 8;

// ---------------- Timing Constants (ms) ----------------
constexpr uint32_t I2C_CLOCK_HZ        = 100000;
constexpr uint32_t RAIL_WAIT_MS        = 1500;
constexpr uint32_t PDN_RESET_MS        = 20;
constexpr uint32_t PDN_SETTLE_MS       = 50;   // Datasheet >= 1 ms; board has C92=10uF AVDRV & C90=2.2uF VCOM
constexpr uint32_t I2S_CLOCK_STABLE_MS = 50;
constexpr uint32_t PLL_LOCK_WAIT_MS    = 50;   // Datasheet max 10 ms
constexpr uint32_t CODEC_SETTLE_MS     = 200;  // 400 us RAM clear + OUT1/2 DC bias charge while muted
constexpr uint32_t AMP_SETTLE_MS       = 500;
constexpr uint32_t UNMUTE_SETTLE_MS    = 20;
constexpr uint32_t DAMUTE_RAMP_MS      = 30;   // Soft mute ramp is ~21.3 ms @ 48 kHz

// ---------------- Bluetooth & Audio Tuning ----------------
constexpr const char *BT_DEVICE_NAME   = "HK Onyx Studio 6";
constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
constexpr uint8_t  DEFAULT_BT_VOLUME   = 80;     // 0..127 AVRCP scale
constexpr int      VOLUME_STEP         = 8;      // ~16 steps across 0..127
constexpr uint32_t DUAL_PAIR_WINDOW_MS = 60000;  // 60 s discoverable window on BLUE press
constexpr uint32_t IDLE_AUTO_OFF_MS    = 15UL * 60UL * 1000UL; // 15 min idle cutoff on battery

// Set to true ONLY if startup chime (low tone first, high chirp second) plays from swapped drivers
constexpr bool     SWAP_WOOFER_TWEETER = false;

// Crossover frequency: 2.5 kHz LR4 matches the 120 mm woofer + 25 mm tweeter without woofer beaming
constexpr float    CROSSOVER_HZ        = 2500.0f; // 4th-order Linkwitz-Riley (-24 dB/oct)
constexpr float    SUBSONIC_HPF_HZ     = 48.0f;   // Mandatory 48 Hz 4th-order protection (FLAT_BASELINE)
constexpr float    MASTER_GAIN         = 1.00f;   // Full scale; EQ_PRE_GAIN_LIN + limiters guarantee zero clipping
constexpr float    CHIME_GAIN          = 0.30f;   // Fixed playback level for Harman ADPCM chimes (0..1)

// ---------------- SysID Capture / Validation Defaults (Part C & D of brief) ----------------
constexpr uint32_t SYSID_SAMPLE_RATE      = 48000;
constexpr float    SYSID_SWEEP_F1_HZ      = 20.0f;
constexpr float    SYSID_SWEEP_F2_HZ      = 20000.0f;
constexpr float    SYSID_SWEEP_DUR_S      = 4.0f;
constexpr float    SYSID_SWEEP_FADE_MS    = 10.0f;
constexpr float    SYSID_SWEEP_LEVEL_DBFS = -20.0f;

// ---------------- Harman-Style DSP Tuning (used by dsp_crossover.cpp) ----------------
constexpr float    DC_BLOCK_HZ         = 15.0f;

// 3-band Global Voicing PEQ (Onyx Studio 5 warm bass, clean mids, crisp presence)
constexpr float    PEQ1_HZ             = 72.0f;
constexpr float    PEQ1_Q              = 1.10f;
constexpr float    PEQ1_DB             = 3.5f;

constexpr float    PEQ2_HZ             = 320.0f;
constexpr float    PEQ2_Q              = 1.40f;
constexpr float    PEQ2_DB             = -2.0f;

constexpr float    PEQ3_HZ             = 3800.0f;
constexpr float    PEQ3_Q              = 1.20f;
constexpr float    PEQ3_DB             = 1.8f;

// Tweeter acoustic voice-coil time alignment (measured directly from m6_t1..4 IR: wf_t - tw_t = +0.15 ms)
constexpr float    TWEETER_DELAY_US    = 150.0f;

// Adaptive ISO 226 Loudness shelves (deep Harman bass at low/mid volume, flattens toward max)
constexpr float    LOUDNESS_LOW_HZ       = 88.0f;
constexpr float    LOUDNESS_HIGH_HZ      = 9000.0f;
constexpr float    LOUDNESS_SHELF_SLOPE  = 0.75f;
constexpr float    LOUDNESS_LOW_MAX_DB   = 7.5f;
constexpr float    LOUDNESS_HIGH_MAX_DB  = 2.0f;
constexpr uint8_t  LOUDNESS_VOL_FULL     = 40;
constexpr uint8_t  LOUDNESS_VOL_FLAT_AC  = 110;
constexpr uint8_t  LOUDNESS_VOL_FLAT_BAT = 100;

// Dynamic 4th-order Butterworth sliding subsonic HPF (40-70 Hz: deep bass when quiet, safe when loud)
constexpr float    SUBSONIC_AC_MIN_HZ    = 40.0f;
constexpr float    SUBSONIC_AC_MAX_HZ    = 62.0f;
constexpr float    SUBSONIC_BAT_MIN_HZ   = 42.0f;
constexpr float    SUBSONIC_BAT_MAX_HZ   = 70.0f;
constexpr float    SUBSONIC_EXCUR_THR    = 0.28f;

// 2-band Look-Ahead Peak + Slow RMS Thermal DRC (separate 19 V AC vs 10 V Battery ceilings)
constexpr float    DRC_LOOKAHEAD_MS      = 1.5f;
constexpr float    DRC_PEAK_RATIO        = 20.0f;
constexpr float    DRC_RMS_RATIO         = 6.0f;
constexpr float    DRC_KNEE_DB           = 6.0f;

constexpr float    WF_PEAK_ATK_MS        = 0.5f;
constexpr float    WF_PEAK_REL_MS        = 80.0f;
constexpr float    WF_RMS_ATK_MS         = 40.0f;
constexpr float    WF_RMS_REL_MS         = 400.0f;

constexpr float    TW_PEAK_ATK_MS        = 0.3f;
constexpr float    TW_PEAK_REL_MS        = 60.0f;
constexpr float    TW_RMS_ATK_MS         = 30.0f;
constexpr float    TW_RMS_REL_MS         = 300.0f;

constexpr float    WF_PEAK_THR_AC_DB     = -2.5f;
constexpr float    WF_RMS_THR_AC_DB      = -6.5f;
constexpr float    TW_PEAK_THR_AC_DB     = -3.5f;
constexpr float    TW_RMS_THR_AC_DB      = -8.5f;

constexpr float    WF_PEAK_THR_BAT_DB    = -7.5f;  // Keeps peak <= ~8.4 Vpk on 10 V battery boost rail
constexpr float    WF_RMS_THR_BAT_DB     = -11.0f;
constexpr float    TW_PEAK_THR_BAT_DB    = -7.5f;
constexpr float    TW_RMS_THR_BAT_DB     = -12.0f;

// AK7755 line-out volume (CONT14 LOVOLx: 0=mute, 1=-28dB .. 0xF=0dB in 2 dB steps)
// Raised +4 dB overall (and +2 dB tweeter air) to match Studio 9 loudness @ 38% phone volume
constexpr uint8_t  AK_WOOFER_VOL       = 0xE;     // OUT1 (DAC L -> woofer):  -2 dB
constexpr uint8_t  AK_TWEETER_VOL      = 0xD;     // OUT2 (DAC R -> tweeter): -4 dB

// Aux-In (AK7755 pin 34 IN1 = AUX_R, pin 32 IN3 = AUX_L -> stereo ADC -> SDOUT3 -> ESP32 D27)
// CONT12 MGNL/MGNR: 0=0dB (2.2 Vpp full scale), 1=+2dB, 2=+4dB, 3=+6dB ...
constexpr uint8_t  AK_AUX_ADC_GAIN     = 0x0;     // 0 dB input amp gain (clean for phone/laptop)
constexpr int16_t  AUX_SENSE_PEAK_THR  = 320;     // ~-40 dBFS auto-sense threshold on pins 34/32
constexpr uint32_t AUX_SENSE_HOLD_MS   = 150;     // Continuous audio needed to auto-enter Aux-In
constexpr uint32_t AUX_SILENCE_EXIT_MS = 4000;    // Silence timeout before auto-returning to BT

// ---------------- Battery & Thermal Protection (1S 3.7 V Li-ion) ----------------
constexpr float    BAT_DIVIDER_SCALE   = 4.0f;    // (300k + 100k) / 100k
constexpr float    BAT_PRESENT_MIN_V   = 1.50f;   // Below this = no battery attached (bench mode)
constexpr float    BAT_LOW_V           = 3.35f;   // Turn on low-battery RED warning
constexpr float    BAT_RECOVER_V       = 3.45f;   // Hysteresis to clear low-battery warning
constexpr float    BAT_CRITICAL_V      = 3.10f;   // Sustained cutoff to protect Li-ion cell
constexpr float    BAT_TEMP_HOT_V      = 0.42f;   // ~65 C on 10k NTC || 4.7k divider
constexpr float    BAT_TEMP_MIN_V      = 0.05f;   // Ignore shorted/unwired 0 V bench readings
constexpr uint32_t BAT_CUTOFF_HOLD_MS  = 5000;    // Must stay critical for 5 s before auto-off

} // namespace BoardConfig
