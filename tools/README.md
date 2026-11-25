# Onyx Studio Acoustic System Identification Toolkit

Offline Python tools matching [DSP_Sysid_Brief_and_Capture_Procedure.pdf](../docs/DSP_Sysid_Brief_and_Capture_Procedure.pdf) (Parts B–E) to measure a reference **Harman Kardon Onyx Studio 9** (`T9`) and your modified ESP32 speaker (`P6`), extract linear EQ + dynamic bass compression (DRC), and generate `include/eq_coeffs.h`.

## 1. Setup (for analysis & fitting)
`generate_signals.py` works with standard Python (zero dependencies). For `analyze_sysid.py` and `fit_biquads.py`, install the requirements in a virtual environment:

```bash
python3 -m venv tools/.venv
source tools/.venv/bin/activate
pip install -r tools/requirements.txt
```

## 2. Generate Test Signals
```bash
python3 tools/generate_signals.py
```
This writes 48 kHz 32-bit float dual-mono WAVs + metadata into `tools/signals/`:
- `ess_stepped.wav` — 6 Farina sweeps (20 Hz–20 kHz, 4 s) at `-24, -18, -12, -6, -3, 0 dBFS`
- `drc_burst_60hz.wav` — 3 repeats of 60 Hz `-24 -> -3 -> -24 dBFS` burst
- `ess_single_m20db.wav` — single `-20 dBFS` Farina sweep (exact match to ESP32 capture mode)
- `signals_meta.json` — sample-accurate timestamps & parameters

## 3. Record Both Speakers (Part B Checklist)
1. Flash the dedicated 48 kHz measurement mode onto your ESP32:
   ```bash
   pio run -e sysid_capture -t upload
   ```
   - Starts in **FLAT BASELINE** mode (solid WHITE LED: mandatory 48 Hz 4th-order HPF + 3.5 kHz LR4 crossover ONLY; EQ & DRC off; `CONT14 = 0xAC` = -6 dB woofer / -10 dB tweeter).
   - Tap **`PLAY/PAUSE`** (D18) → 3-second RED countdown → plays exact `ess_single_m20db` sweep at 48 kHz.
   - Tap **`BLUE`** (D32) to toggle between **FLAT** (solid WHITE) and **TUNED** (breathing WHITE) for post-tuning validation without moving the speaker.
2. Record the Studio 9 (`s9_noise.wav`, `s9_ess_stepped.wav`, `s9_drc_burst.wav`) and the modified speaker (`m6_noise.wav`, `m6_ess_m20_take1..3.wav`) with the **exact same phone position (1.0 m) and locked manual gain** (48 kHz, mono, uncompressed WAV, raw/unprocessed mic). Optionally also capture near-field woofer sweeps (`s9_nf.wav`, `m6_nf.wav`).

## 4. Analyze Recordings
```bash
python3 tools/analyze_sysid.py \
  --meta tools/signals/signals_meta.json \
  --s9-stepped recordings/s9_ess_stepped.wav \
  --s9-burst recordings/s9_drc_burst.wav \
  --s9-noise recordings/s9_noise.wav \
  --m6-takes recordings/m6_ess_m20_take1.wav recordings/m6_ess_m20_take2.wav recordings/m6_ess_m20_take3.wav \
  --m6-noise recordings/m6_noise.wav \
  --out-dir tools/results
```
*(Add `--s9-nf recordings/s9_nf.wav --m6-nf recordings/m6_nf.wav` if you recorded near-field bass sweeps.)*

Review `tools/results/plot_responses.png`, `plot_drc_gr.png`, and `plot_drc_burst.png`.

## 5. Fit Biquads & Generate `include/eq_coeffs.h`
```bash
python3 tools/fit_biquads.py \
  --results tools/results/sysid_results.json \
  --sections 8 \
  --out-header include/eq_coeffs.h
```
Then flash the Bluetooth firmware (`pio run -e esp32dev -t upload`) or `sysid_capture` (press `BLUE` to switch to **TUNED** mode) and pass `--validate recordings/m6_tuned.wav` to `analyze_sysid.py` to verify the match against `T9`!
