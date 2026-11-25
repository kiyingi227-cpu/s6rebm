# DSP Architecture, Acoustic SysID Findings & Tuning Log

This document records the complete acoustic measurement workflow comparing a factory **Harman Kardon Onyx Studio 9 (`T9`)** against the **ESP32-modified Onyx Studio 5/6 (`P6`)**, every root cause uncovered, the current `v1.0.6` Core 1 DSP pipeline, and how to fine-tune or validate future revisions.

---

## 1. Current Core 1 DSP Pipeline (`v1.0.6`)

Runs entirely on dedicated Core 1 task `dsp_core1` (`src/audio_output.cpp` + `src/dsp_crossover.cpp`), isolated from Bluedroid on Core 0 via a lock-free 4096-frame SPSC ring buffer.

```
Stereo PCM16 (L, R)
  │
  ├──► Mid  = 0.5*(L+R) ──► 15 Hz DC Blocker ───────────────────────────┐
  └──► Side = 0.5*(L-R) ──► SpatialDiffuser (400 Hz HPF → 2× Schroeder  ├─► (+) × smoothVol (norm^1.35, 45 ms slew)
                             All-Pass 3.73/6.61 ms + 13.81 ms warm ER) ─┘         │
                                                                                  ▼
                                                            [Signal + AVRCP ISO 226 Loudness Shelves]
                                                            (< 88 Hz up to +7.5 dB, > 9 kHz up to +2.0 dB)
                                                                                  │
                                                                                  ▼
                                                            [EQ_PRE_GAIN (-4.42 dB) → 5-Band Studio 9 EQ]
                                                                                  │
                                                                                  ▼
                                                            [Studio 9 Dynamic Bass Stage (92.1 Hz split)]
                                                            (63.6 ms atk / 1085.7 ms rel, max -6.5 dB GR)
                                                                                  │
                                           ┌──────────────────────────────────────┴──────────────────────────────────────┐
                                           ▼                                                                             ▼
                          [2.5 kHz LR4 LPF (Woofer)]                                                  [2.5 kHz LR4 HPF (Tweeter)]
                                           │                                                                             │
                                           ▼                                                                             ▼
                 [64-bit Sliding Subsonic HPF (40..70 Hz)]                                    [150 µs Acoustic Delay Line]
                 (4th-order Butterworth, < 90 Hz excursion tap)                               (measured wf_t - tw_t = +0.15 ms)
                                           │                                                                             │
                                           ▼                                                                             ▼
                    [Look-Ahead Peak + RMS BandLimiter]                                         [Look-Ahead Peak + RMS BandLimiter]
                    (19 V AC vs 10 V Battery ceilings)                                          (19 V AC vs 10 V Battery ceilings)
                                           │                                                                             │
                                           ▼                                                                             ▼
                               [C1 Soft-Clip Safety]                                                       [C1 Soft-Clip Safety]
                                           │                                                                             │
                                           ▼                                                                             ▼
                          I2S0 L → AK7755 OUT1 (-2 dB, 0xE)                                           I2S0 R → AK7755 OUT2 (-4 dB, 0xD)
                                   → TPA3116 U19 (Woofer)                                                     → TPA3116 U20 (Tweeter)
```

---

## 2. Acoustic System Identification Findings (`Studio 9` vs `Modified 6`)

All recordings were captured at `48 kHz / 16-bit mono` with locked microphone gain (`-63.1` to `-63.7 dBFS` room noise floor) in [`tools/recodings/`](tools/recodings/) and [`tools/recodings/m6_sample2/`](tools/recodings/m6_sample2/).

### Key Acoustic Measurements
1. **Studio 9 Dynamic Bass Compression (`s9_ess_stepped.wav` + `s9_drc_burst.wav`):**
   - Clock drift `+25.8 ppm` corrected to `0.00 ppm`.
   - Linear at `1 kHz` within `±0.1 dB` from `-24` to `-6 dBFS`, with gentle `-1.3 dB` broadband limiting at `0 dBFS`.
   - Dynamic bass shelf centered at `50–60 Hz` with corner **`~92.1 Hz`**, smoothly pulling bass back by `-0.8 dB` (`-18 dBFS`), `-2.0 dB` (`-12`), `-2.9 dB` (`-6`), `-3.5 dB` (`-3`), and **`-6.1 dB` (`0 dBFS`)**.
   - 60 Hz burst Hilbert envelope: **`attack τ = 63.6 ms`**, **`release τ = 1085.7 ms`** (see [`tools/results/plot_drc_gr.png`](../tools/results/plot_drc_gr.png) and [`tools/results/plot_drc_burst.png`](../tools/results/plot_drc_burst.png)).

2. **Reflection-Free Near-Field Woofer Comparison (`s9_nf.wav` vs `m6_sample2/nf.wav`, 2 cm, 30 ms gate):**
   - Studio 9 extends **`+4.4 dB` deeper at `50 Hz`**, `+2.0 dB` at `63 Hz`, `+1.7 dB` at `80 Hz`.
   - From `125 Hz – 1000 Hz`, both woofers track closely, with Studio 9 slightly cleaner (`-1.0 to -2.0 dB`) around `150–500 Hz`.

3. **Driver Time Alignment & Crossover (`m6_sample2/take1..4.wav`):**
   - Measured Hilbert group-delay arrival offset across all 4 takes: **`wf_t - tw_t = +0.15 ms` (`150 µs` ≈ `5.1 cm` voice-coil depth)**, electrically in-phase.
   - At `3.5 kHz`, the 120 mm woofer exhibited a `-4` to `-7 dB` beaming dip (`2.8–3.1 kHz`). Moving the LR4 crossover to **`2.5 kHz`** with **`150 µs`** tweeter delay aligns both drivers cleanly before woofer beaming starts.

4. **In-Room Dispersion & "Surround Feel" (`80 ms` power response + `3–60 ms` Room-to-Direct ratio):**
   - Relative to `100–600 Hz` body, Studio 9's 3-tweeter Constant Sound Field (CSF) array radiates **`+3` to `+8.6 dB` more in-room energy from `1 kHz – 12.5 kHz`** and **`+2.2` to `+2.8 dB` higher early-room reflection energy (`1–5 kHz`)**.
   - Simple mono downmix `0.5*(L+R)` deleted stereo `L - R` room ambience; adding `SpatialDiffuser` (`> 400 Hz` all-pass decorrelation + `13.8 ms` damped reflection on `L - R`) restores spacious 3D depth without affecting bass or mono test signals.

---

## 3. Current DSP Parameters (`v1.0.6`)

### 5-Band Studio 9 Open-Air EQ ([`include/eq_coeffs.h`](../include/eq_coeffs.h))
Pre-gain: **`-4.42 dB`** (`0.6011`), max pole radius: **`0.9967 < 1.0`**.

| Band | `f0` | `Q` | Gain | Acoustic Role |
|---|---|---|---|---|
| 0 | `54.0 Hz` | `0.85` | `+4.0 dB` | Deep Harman sub-bass hammer (`40–80 Hz`) |
| 1 | `240.0 Hz` | `1.10` | `-2.0 dB` | Clean, tight upper-bass / anti-boxiness |
| 2 | `1550.0 Hz` | `0.70` | `+3.5 dB` | Restores Studio 9 open upper-mid presence |
| 3 | `5200.0 Hz` | `0.80` | `+2.5 dB` | Restores Studio 9 `4–6.5 kHz` CSF openness |
| 4 | `11500.0 Hz` | `0.70` | `+2.5 dB` | Restores silky top-end air & sparkle |

### Dynamic Loudness, Subsonic HPF, DRC & Gain Staging ([`include/board_config.h`](../include/board_config.h))
- **AK7755 Analog DAC (`CONT14`):** Woofer `OUT1 = 0xE (-2 dB)`, Tweeter `OUT2 = 0xD (-4 dB)`.
- **Volume Curve:** `MASTER_GAIN * powf(avrcpVol / 127.0f, 1.35f)` with `45 ms` per-sample slew (`+9.9 dB` louder at 38% phone volume vs old `norm²` curve, `~2 dB` smooth step per notch).
- **Dynamic Loudness EQ:** Combines AVRCP volume (`FULL=40`, `FLAT_AC=110`, `FLAT_BAT=100`) and post-volume program envelope (`-32` to `-12 dBFS`, capped at `0.35` when `vol ≥ 105`), applying up to **`+7.5 dB` below `88 Hz`** and **`+2.0 dB` above `9 kHz`** at low volume (works on both Bluetooth and Aux-In).
- **Sliding Subsonic HPF (64-bit `Biquad64`):** `40 → 62 Hz` on 19 V AC, `42 → 70 Hz` on 10 V Battery, driven by `< 90 Hz` excursion detector (`SUBSONIC_EXCUR_THR = 0.28`).
- **Studio 9 Dynamic Bass DRC:** `92.14 Hz` 1-pole complementary split (`low + rest == x` at 0 dB GR), `63.6 ms` attack, `1085.7 ms` release, soft-knee `2.5:1 / 10 dB`, `-16 dBFS` AC / `-19 dBFS` Bat threshold, max `-6.5 dB` GR.
- **2-Band Look-Ahead Limiters:** `1.5 ms` look-ahead; AC peak/RMS `-2.5/-6.5 dB` (woofer) & `-3.5/-8.5 dB` (tweeter); Battery `-6.5/-10.5 dB` (woofer) & `-7.5/-12.0 dB` (tweeter).

---

## 4. Firmware Version & Fix Log

| Version | Key Changes & Bugs Fixed |
|---|---|
| **`v1.0.0`** | Initial modular firmware: fixed inverted `MUTE` polarity (`LOW`=muted) & `akVerify()` mismatch, SLG4V4826 power-button shutdown (`D17`), Bluetooth Classic A2DP + AVRCP + Dual-Pair, PWM LEDs, battery monitor, Dual-Mode BT OTA. |
| **`v1.0.1`** | Dedicated Core 1 DSP task (`dsp_core1` + 4096-frame SPSC ring), full-duplex I2S Aux-In (`IN1/IN3` pins 34/32 → `SDOUT3` → `D27`), disabled automatic self-power-off triggers per user request. |
| **`v1.0.2`** | Added OTA-safe in-firmware 48 kHz SysID capture mode on working buttons (`BLUE` 3× / `BLUE+PLAY` hold; worked around non-functional `VOL+/VOL-` buttons). |
| **`v1.0.3`** | **Fixed SysID sweep I2S DMA underrun:** `double exp()+sin()` in `playSysidSingleSweep()` caused 5.3 ms DMA underruns stretching 4.0 s sweeps to 7.1 s. Replaced with 1024-entry interpolated sine LUT + trapezoidal phase recurrence (`108 dB` SNR, zero underruns). Upgraded `analyze_sysid.py` with timing guard & 35 ms bass DRC gating. |
| **`v1.0.4`** | First 8-biquad SysID fit (`RMS = 1.23 dB`) + Studio 9 `92.1 Hz` Dynamic Bass DRC. *Listening review revealed:* bypassed low-volume dynamic loudness + fixed 48 Hz HPF + 1 m table-bounce contamination causing hollowed lower-mids & nasal 1.7 kHz boost. |
| **`v1.0.5`** | Re-enabled volume- + signal-aware ISO 226 Dynamic Loudness (`+8 dB < 88 Hz`), sliding `40–70 Hz` subsonic HPF, `2.5 kHz` LR4 crossover + measured `150 µs` tweeter delay, and reflection-free Harman voicing. Restored deep bass hammer at low volume. |
| **`v1.0.6`** | Re-analyzed 80 ms in-room power response & dispersion; added `SpatialDiffuser` (Mid/Side `L-R` Schroeder all-pass + warm early reflection for Harman CSF surround feel); restored `1.5–12 kHz` open air EQ; fixed AVRCP 1.6 `SET_ABSOLUTE_VOLUME` echo bug that made phone volume sliders skip; raised AK7755 DAC gain (`0xE`/`0xD`) + smooth `norm^1.35` volume curve matching Studio 9 loudness at 38%. |
| **`v1.0.7`** | **Current:** Added factory-style **Off-State Charging Mode** (`PowerManager::isChargerOnlyWake()` + `waitForPowerButtonWithChargingLed()`): detects `19 V` charger-only wake (`VREG_EN == LOW && DC_DET == HIGH`) and power-off while plugged in; keeps amps/AK7755/BT off, smoothly breathes RED LED while `bq24090 /CHG` is active (turns OFF when 100% full), wakes immediately on Power Button press, and bypasses standby on `ESP_RST_SW` so OTA reboots always confirm cleanly. |

---

## 5. Quick Tuning & Validation Cheat Sheet

- **Adjust "surround / room feel" strength:** `SpatialDiffuser::processSide()` return weights in [`src/dsp_crossover.cpp`](../src/dsp_crossover.cpp) (`0.28f * y2 + 0.14f * er`).
- **Adjust low-volume deep bass:** `LOUDNESS_LOW_MAX_DB` (`7.5f`) and `LOUDNESS_LOW_HZ` (`88.0f`) in [`include/board_config.h`](../include/board_config.h).
- **Adjust volume curve / 38% loudness:** Exponent `1.35f` in `AudioCrossover::setVolume()` and `AK_WOOFER_VOL` / `AK_TWEETER_VOL` (`0xE` / `0xD`).
- **Pre-bundled `esp-dsp` ready for future upgrades:** `#include "esp_dsp.h"` is already available in the ESP32 SDK (`dsps_fir_f32_ae32`, `dsps_biquad_f32_ae32`, FFT) for future FIR phase correction or MaxxBass-style virtual sub-bass synthesis.
- **Re-run Python SysID analysis / validation sweep:**
  ```bash
  tools/.venv/bin/python tools/analyze_sysid.py \
    --meta tools/signals/signals_meta.json \
    --s9-stepped tools/recodings/s9_ess_stepped.wav \
    --s9-burst tools/recodings/s9_drc_burst.wav \
    --s9-noise tools/recodings/s9_noise.wav \
    --m6-takes tools/recodings/m6_sample2/take1.wav tools/recodings/m6_sample2/take2.wav \
               tools/recodings/m6_sample2/take3.wav tools/recodings/m6_sample2/take4.wav \
    --m6-noise tools/recodings/m6_sample2/m6_silence.wav \
    --s9-nf tools/recodings/s9_nf.wav \
    --m6-nf tools/recodings/m6_sample2/nf.wav \
    --validate tools/recodings/m6_sample2/m6_tuned.wav \
    --out-dir tools/results
  ```
