#!/usr/bin/env python3
"""
Generate 48 kHz 32-bit float dual-mono WAV test signals and signals_meta.json
for Harman Kardon Onyx Studio acoustic system identification (Part C1 of brief).

Works with numpy+soundfile if installed, or pure Python stdlib as a fallback so
test WAVs can be generated immediately on any machine.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
from pathlib import Path
from typing import List, Tuple

SAMPLE_RATE = 48000
SWEEP_F1_HZ = 20.0
SWEEP_F2_HZ = 20000.0
SWEEP_DUR_S = 4.0
SWEEP_FADE_MS = 10.0

MARKER_FREQ_HZ = 1000.0
MARKER_LEVEL_DBFS = -12.0
MARKER_BURST_MS = 50.0
MARKER_GAP_MS = 50.0
MARKER_COUNT = 3
MARKER_FADE_MS = 5.0

STEPPED_LEVELS_DBFS = [-24.0, -18.0, -12.0, -6.0, -3.0, 0.0]
SINGLE_LEVEL_DBFS = -20.0

DRC_FREQ_HZ = 60.0
DRC_LOW_DBFS = -24.0
DRC_HIGH_DBFS = -3.0
DRC_PRE_S = 1.5
DRC_BURST_S = 0.5
DRC_POST_S = 2.0
DRC_GAP_S = 3.0
DRC_REPEATS = 3
DRC_RAMP_MS = 2.0


def db_to_lin(db: float) -> float:
    return 10.0 ** (db / 20.0)


def apply_cosine_fades(samples: List[float], fade_samples: int) -> None:
    n = len(samples)
    f = min(fade_samples, n // 2)
    for i in range(f):
        w = 0.5 * (1.0 - math.cos(math.pi * i / f))
        samples[i] *= w
        samples[n - 1 - i] *= w


def make_silence(seconds: float) -> List[float]:
    return [0.0] * int(round(seconds * SAMPLE_RATE))


def make_start_marker() -> List[float]:
    amp = db_to_lin(MARKER_LEVEL_DBFS)
    burst_n = int(round(MARKER_BURST_MS * 1e-3 * SAMPLE_RATE))
    gap_n = int(round(MARKER_GAP_MS * 1e-3 * SAMPLE_RATE))
    fade_n = int(round(MARKER_FADE_MS * 1e-3 * SAMPLE_RATE))
    two_pi_f = 2.0 * math.pi * MARKER_FREQ_HZ / SAMPLE_RATE

    out: List[float] = []
    for k in range(MARKER_COUNT):
        burst = [amp * math.sin(two_pi_f * i) for i in range(burst_n)]
        apply_cosine_fades(burst, fade_n)
        out.extend(burst)
        if k < MARKER_COUNT - 1:
            out.extend([0.0] * gap_n)
    return out


def make_farina_ess(level_dbfs: float) -> List[float]:
    """Farina exponential sine sweep: x(t) = sin(K * (exp(t/L) - 1))."""
    amp = db_to_lin(level_dbfs)
    n = int(round(SWEEP_DUR_S * SAMPLE_RATE))
    L = SWEEP_DUR_S / math.log(SWEEP_F2_HZ / SWEEP_F1_HZ)
    K = 2.0 * math.pi * SWEEP_F1_HZ * L
    inv_fs = 1.0 / SAMPLE_RATE

    samples = [amp * math.sin(K * (math.exp((i * inv_fs) / L) - 1.0)) for i in range(n)]
    fade_n = int(round(SWEEP_FADE_MS * 1e-3 * SAMPLE_RATE))
    apply_cosine_fades(samples, fade_n)
    return samples


def write_wav_float32_dual_mono(path: Path, mono: List[float], fs: int = SAMPLE_RATE) -> None:
    """Write 2-channel identical dual-mono 32-bit IEEE float WAV."""
    n_frames = len(mono)
    n_ch = 2
    bits = 32
    byte_rate = fs * n_ch * (bits // 8)
    block_align = n_ch * (bits // 8)
    data_size = n_frames * block_align

    # RIFF header + fmt chunk (IEEE float, format=3) + fact chunk + data chunk
    riff_size = 4 + (8 + 18) + (8 + 4) + (8 + data_size)

    with open(path, "wb") as f:
        f.write(struct.pack("<4sI4s", b"RIFF", riff_size, b"WAVE"))
        f.write(struct.pack("<4sIHHIIHHH", b"fmt ", 18, 3, n_ch, fs, byte_rate, block_align, bits, 0))
        f.write(struct.pack("<4sII", b"fact", 4, n_frames))
        f.write(struct.pack("<4sI", b"data", data_size))

        # Write in chunks for speed and low memory overhead
        chunk = 8192
        for start in range(0, n_frames, chunk):
            end = min(start + chunk, n_frames)
            interleaved = [0.0] * (2 * (end - start))
            for idx, v in enumerate(mono[start:end]):
                interleaved[2 * idx] = v
                interleaved[2 * idx + 1] = v
            f.write(struct.pack(f"<{len(interleaved)}f", *interleaved))


def build_ess_stepped() -> Tuple[List[float], dict]:
    sig: List[float] = []
    sig.extend(make_silence(2.0))
    marker_start = len(sig)
    marker = make_start_marker()
    sig.extend(marker)
    sig.extend(make_silence(1.0))

    sweeps_meta = []
    for idx, lvl in enumerate(STEPPED_LEVELS_DBFS):
        start_samp = len(sig)
        sw = make_farina_ess(lvl)
        sig.extend(sw)
        sweeps_meta.append({
            "index": idx,
            "level_dbfs": lvl,
            "start_sample": start_samp,
            "length_samples": len(sw),
            "start_s": round(start_samp / SAMPLE_RATE, 6),
        })
        sig.extend(make_silence(1.0))

    meta = {
        "filename": "ess_stepped.wav",
        "total_samples": len(sig),
        "duration_s": round(len(sig) / SAMPLE_RATE, 6),
        "marker_start_sample": marker_start,
        "marker_length_samples": len(marker),
        "sweeps": sweeps_meta,
    }
    return sig, meta


def build_ess_single() -> Tuple[List[float], dict]:
    sig: List[float] = []
    sig.extend(make_silence(1.0))
    marker_start = len(sig)
    marker = make_start_marker()
    sig.extend(marker)
    sig.extend(make_silence(1.0))

    start_samp = len(sig)
    sw = make_farina_ess(SINGLE_LEVEL_DBFS)
    sig.extend(sw)
    sig.extend(make_silence(1.0))

    meta = {
        "filename": "ess_single_m20db.wav",
        "total_samples": len(sig),
        "duration_s": round(len(sig) / SAMPLE_RATE, 6),
        "marker_start_sample": marker_start,
        "marker_length_samples": len(marker),
        "sweeps": [{
            "index": 0,
            "level_dbfs": SINGLE_LEVEL_DBFS,
            "start_sample": start_samp,
            "length_samples": len(sw),
            "start_s": round(start_samp / SAMPLE_RATE, 6),
        }],
    }
    return sig, meta


def build_drc_burst() -> Tuple[List[float], dict]:
    sig: List[float] = []
    sig.extend(make_silence(1.0))
    marker_start = len(sig)
    marker = make_start_marker()
    sig.extend(marker)
    sig.extend(make_silence(1.0))

    amp_lo = db_to_lin(DRC_LOW_DBFS)
    amp_hi = db_to_lin(DRC_HIGH_DBFS)
    ramp_n = max(1, int(round(DRC_RAMP_MS * 1e-3 * SAMPLE_RATE)))
    pre_n = int(round(DRC_PRE_S * SAMPLE_RATE))
    burst_n = int(round(DRC_BURST_S * SAMPLE_RATE))
    post_n = int(round(DRC_POST_S * SAMPLE_RATE))
    gap_n = int(round(DRC_GAP_S * SAMPLE_RATE))
    fade_edge_n = int(round(10.0e-3 * SAMPLE_RATE))

    # Build continuous envelope for the 60 Hz carrier across all 3 repeats
    carrier_start = len(sig)
    env: List[float] = []
    bursts_meta = []

    for r in range(DRC_REPEATS):
        env.extend([amp_lo] * pre_n)
        step_up_sample = carrier_start + len(env)
        for i in range(ramp_n):
            w = 0.5 * (1.0 - math.cos(math.pi * i / ramp_n))
            env.append(amp_lo + w * (amp_hi - amp_lo))
        env.extend([amp_hi] * (burst_n - ramp_n))

        step_down_sample = carrier_start + len(env)
        for i in range(ramp_n):
            w = 0.5 * (1.0 - math.cos(math.pi * i / ramp_n))
            env.append(amp_hi + w * (amp_lo - amp_hi))
        env.extend([amp_lo] * (post_n - ramp_n))

        bursts_meta.append({
            "repeat": r,
            "step_up_sample": step_up_sample,
            "step_down_sample": step_down_sample,
            "step_up_s": round(step_up_sample / SAMPLE_RATE, 6),
            "step_down_s": round(step_down_sample / SAMPLE_RATE, 6),
            "low_dbfs": DRC_LOW_DBFS,
            "high_dbfs": DRC_HIGH_DBFS,
            "burst_s": DRC_BURST_S,
            "post_s": DRC_POST_S,
        })

        if r < DRC_REPEATS - 1:
            env.extend([amp_lo] * gap_n)

    # Fade carrier in/out gently at start and end of burst sequence
    apply_cosine_fades(env, fade_edge_n)
    two_pi_f = 2.0 * math.pi * DRC_FREQ_HZ / SAMPLE_RATE
    carrier = [env[i] * math.sin(two_pi_f * i) for i in range(len(env))]
    sig.extend(carrier)
    sig.extend(make_silence(1.0))

    meta = {
        "filename": "drc_burst_60hz.wav",
        "total_samples": len(sig),
        "duration_s": round(len(sig) / SAMPLE_RATE, 6),
        "marker_start_sample": marker_start,
        "marker_length_samples": len(marker),
        "carrier_freq_hz": DRC_FREQ_HZ,
        "carrier_start_sample": carrier_start,
        "ramp_ms": DRC_RAMP_MS,
        "bursts": bursts_meta,
    }
    return sig, meta


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate 48 kHz float32 SysID WAV test signals.")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "signals",
        help="Output directory for WAV files and signals_meta.json",
    )
    args = parser.parse_args()
    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    stepped_sig, stepped_meta = build_ess_stepped()
    single_sig, single_meta = build_ess_single()
    burst_sig, burst_meta = build_drc_burst()

    write_wav_float32_dual_mono(out_dir / "ess_stepped.wav", stepped_sig)
    write_wav_float32_dual_mono(out_dir / "ess_single_m20db.wav", single_sig)
    write_wav_float32_dual_mono(out_dir / "drc_burst_60hz.wav", burst_sig)

    full_meta = {
        "sample_rate": SAMPLE_RATE,
        "channels": 2,
        "format": "WAV_IEEE_FLOAT32_DUAL_MONO",
        "sweep_params": {
            "type": "farina_ess",
            "f1_hz": SWEEP_F1_HZ,
            "f2_hz": SWEEP_F2_HZ,
            "duration_s": SWEEP_DUR_S,
            "fade_ms": SWEEP_FADE_MS,
            "L": SWEEP_DUR_S / math.log(SWEEP_F2_HZ / SWEEP_F1_HZ),
        },
        "marker_params": {
            "freq_hz": MARKER_FREQ_HZ,
            "level_dbfs": MARKER_LEVEL_DBFS,
            "burst_ms": MARKER_BURST_MS,
            "gap_ms": MARKER_GAP_MS,
            "count": MARKER_COUNT,
            "fade_ms": MARKER_FADE_MS,
        },
        "hardware_baseline_note": {
            "esp32_hpf_hz": 48.0,
            "esp32_hpf_order": 4,
            "esp32_crossover_hz": 3500.0,
            "esp32_crossover_type": "LR4",
            "ak7755_cont14": "0xAC (OUT1 woofer -6 dB, OUT2 tweeter -10 dB)",
            "tpa3116_gain_db": 26.0,
        },
        "files": {
            "ess_stepped": stepped_meta,
            "ess_single_m20db": single_meta,
            "drc_burst_60hz": burst_meta,
        },
    }

    meta_path = out_dir / "signals_meta.json"
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(full_meta, f, indent=2)

    print(f"Generated signals in {out_dir}:")
    for name in ["ess_stepped.wav", "ess_single_m20db.wav", "drc_burst_60hz.wav", "signals_meta.json"]:
        p = out_dir / name
        print(f"  {p.name:24s}  {os.path.getsize(p):10,d} bytes")


if __name__ == "__main__":
    main()
