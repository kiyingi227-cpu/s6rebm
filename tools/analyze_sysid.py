#!/usr/bin/env python3
"""
Acoustic System Identification Analyzer (Part C2, C3, C4 of brief).

Aligns Studio 9 (T9) and Modified Speaker (P6) recordings, corrects clock drift,
performs regularized Farina deconvolution, separates harmonic distortion, applies
time-gating (+ optional near-field bass splice), computes H_req(f) = |T9| / |P6|,
and extracts Studio 9 dynamic bass compression (DRC) parameters + Hilbert attack/release
time constants.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf
from scipy import signal
from scipy.optimize import least_squares


def load_mono_48k(path: Path, target_fs: int = 48000) -> np.ndarray:
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    mono = np.mean(x, axis=1)
    mono -= np.mean(mono)
    if fs != target_fs:
        g = np.gcd(int(fs), int(target_fs))
        mono = signal.resample_poly(mono, target_fs // g, fs // g)
    return mono


def make_farina_sweep_and_inv(fs: int, f1: float, f2: float, dur_s: float, fade_ms: float) -> Tuple[np.ndarray, np.ndarray, float]:
    n = int(round(dur_s * fs))
    t = np.arange(n, dtype=np.float64) / fs
    L = dur_s / np.log(f2 / f1)
    K = 2.0 * np.pi * f1 * L
    sw = np.sin(K * (np.exp(t / L) - 1.0))

    fade_n = int(round(fade_ms * 1e-3 * fs))
    w = 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_n) / fade_n))
    sw[:fade_n] *= w
    sw[-fade_n:] *= w[::-1]

    # Farina inverse filter: time-reversed sweep scaled by exp(-t/L) (-6 dB/oct)
    inv = sw[::-1] * np.exp(-t / L)
    # Normalize inverse so convolving sw with inv yields 0 dB gain at 1 kHz
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    H_ref = np.fft.rfft(sw, nfft) * np.fft.rfft(inv, nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)
    idx_1k = int(np.argmin(np.abs(freqs - 1000.0)))
    inv /= np.abs(H_ref[idx_1k])
    return sw, inv, L


def deconvolve_regularized(rec_seg: np.ndarray, sw: np.ndarray, fs: int, f1: float, f2: float) -> np.ndarray:
    """FFT deconvolution with band-limited Kirkeby/Tikhonov regularization."""
    n = len(rec_seg)
    nfft = 1 << int(np.ceil(np.log2(n + len(sw))))
    X = np.fft.rfft(sw, nfft)
    Y = np.fft.rfft(rec_seg, nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)

    # Regularization penalty: tiny in-band (f1..f2), larger out-of-band so noise isn't amplified
    eps_in = 1e-5 * np.max(np.abs(X) ** 2)
    eps_out = 1e-1 * np.max(np.abs(X) ** 2)
    reg = np.full_like(freqs, eps_in)
    reg[freqs < f1 * 0.9] = eps_out
    reg[freqs > f2 * 1.05] = eps_out

    H = (Y * np.conj(X)) / (np.abs(X) ** 2 + reg)
    h = np.fft.irfft(H, nfft)
    # Circular shift so t=0 (direct peak) is centered nicely
    return np.roll(h, nfft // 2)


def locate_sweeps(rec: np.ndarray, sw: np.ndarray, expected_starts: List[int], fs: int) -> Tuple[List[int], float]:
    """Find sweep start indices via FFT cross-correlation and estimate clock drift in ppm."""
    nfft = 1 << int(np.ceil(np.log2(len(rec) + len(sw))))
    xc = np.fft.irfft(np.fft.rfft(rec, nfft) * np.conj(np.fft.rfft(sw, nfft)), nfft)[: len(rec)]
    env = np.abs(xc)

    # Anchor on the loudest sweep (usually the last/0 dBFS one)
    best_peak = int(np.argmax(env))
    ref_expected = expected_starts[-1]
    coarse_offset = best_peak - ref_expected

    found = []
    search_rad = int(0.15 * fs)
    for exp_s in expected_starts:
        center = exp_s + coarse_offset
        lo = max(0, center - search_rad)
        hi = min(len(env), center + search_rad)
        if hi <= lo:
            found.append(max(0, min(len(rec) - len(sw), center)))
        else:
            p = lo + int(np.argmax(env[lo:hi]))
            found.append(p)

    ppm = 0.0
    if len(expected_starts) >= 2:
        exp_span = expected_starts[-1] - expected_starts[0]
        meas_span = found[-1] - found[0]
        if exp_span > 0:
            ppm = 1e6 * (meas_span - exp_span) / exp_span
    return found, float(ppm)


def correct_clock_drift(rec: np.ndarray, ppm: float) -> np.ndarray:
    if abs(ppm) <= 5.0:
        return rec
    n = len(rec)
    scale = 1.0 + ppm * 1e-6
    t_new = np.arange(int(round(n / scale)), dtype=np.float64) * scale
    t_new = t_new[t_new < n - 1]
    return np.interp(t_new, np.arange(n, dtype=np.float64), rec)


def gate_ir(ir: np.ndarray, fs: int, gate_ms: float = 5.0, pre_ms: float = 1.0) -> Tuple[np.ndarray, int]:
    """Time-gate around direct impulse peak with half-Hann taper."""
    peak_idx = int(np.argmax(np.abs(ir)))
    pre_n = int(round(pre_ms * 1e-3 * fs))
    post_n = int(round(gate_ms * 1e-3 * fs))
    lo = max(0, peak_idx - pre_n)
    hi = min(len(ir), peak_idx + post_n)

    seg = ir[lo:hi].copy()
    fade_pre = min(int(round(0.3e-3 * fs)), pre_n)
    fade_post = min(int(round(1.0e-3 * fs)), max(1, post_n // 3))
    if fade_pre > 1:
        seg[:fade_pre] *= 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_pre) / fade_pre))
    if fade_post > 1:
        seg[-fade_post:] *= 0.5 * (1.0 + np.cos(np.pi * np.arange(fade_post) / fade_post))
    return seg, peak_idx


def fractional_octave_smooth(freqs: np.ndarray, mag_db: np.ndarray, frac: float = 6.0) -> np.ndarray:
    """1/frac-octave smoothing on magnitude in dB."""
    out = np.copy(mag_db)
    half_bw = 2.0 ** (1.0 / (2.0 * frac))
    for i, f in enumerate(freqs):
        if f <= 0.0:
            continue
        m = (freqs >= f / half_bw) & (freqs <= f * half_bw)
        if np.any(m):
            out[i] = float(np.mean(mag_db[m]))
    return out


def ir_to_response_db(gated_ir: np.ndarray, fs: int, grid_freqs: np.ndarray, smooth_frac: float = 6.0) -> np.ndarray:
    nfft = 16384
    spec = np.fft.rfft(gated_ir, nfft)
    f_fft = np.fft.rfftfreq(nfft, 1.0 / fs)
    mag_db = 20.0 * np.log10(np.maximum(np.abs(spec), 1e-12))
    interp_db = np.interp(grid_freqs, f_fft, mag_db)
    return fractional_octave_smooth(grid_freqs, interp_db, smooth_frac)


def measure_harmonic_ratio_db(ir: np.ndarray, peak_idx: int, L: float, fs: int, order: int) -> float:
    """Measure k-th harmonic pulse energy relative to linear impulse energy (Farina method)."""
    shift = int(round(L * np.log(order) * fs))
    h_idx = peak_idx - shift
    rad = int(round(3.0e-3 * fs))
    if h_idx - rad < 0 or h_idx + rad >= len(ir):
        return -99.0
    lin_e = np.sum(ir[max(0, peak_idx - rad) : min(len(ir), peak_idx + rad)] ** 2)
    har_e = np.sum(ir[h_idx - rad : h_idx + rad] ** 2)
    return float(10.0 * np.log10(max(har_e, 1e-20) / max(lin_e, 1e-20)))


def splice_nearfield(grid_freqs: np.ndarray, gated_db: np.ndarray, nf_db: np.ndarray,
                     f_lo: float = 550.0, f_hi: float = 950.0) -> np.ndarray:
    """Level-match near-field bass to gated 1 m curve in [f_lo, f_hi] and cosine crossfade."""
    ov_match = (grid_freqs >= 600.0) & (grid_freqs <= 1100.0)
    offset = float(np.median(gated_db[ov_match] - nf_db[ov_match]))
    nf_aligned = nf_db + offset

    ov = (grid_freqs >= f_lo) & (grid_freqs <= f_hi)
    out = np.copy(gated_db)
    out[grid_freqs < f_lo] = nf_aligned[grid_freqs < f_lo]
    t = (np.log2(grid_freqs[ov]) - np.log2(f_lo)) / (np.log2(f_hi) - np.log2(f_lo))
    w = 0.5 * (1.0 - np.cos(np.pi * t))
    out[ov] = (1.0 - w) * nf_aligned[ov] + w * gated_db[ov]
    return out


def compute_noise_psd_db(noise_rec: Optional[np.ndarray], fs: int, grid_freqs: np.ndarray) -> np.ndarray:
    if noise_rec is None or len(noise_rec) < 4096:
        return np.full_like(grid_freqs, -120.0)
    f_psd, pxx = signal.welch(noise_rec, fs=fs, nperseg=4096)
    db = 10.0 * np.log10(np.maximum(pxx, 1e-18))
    return np.interp(grid_freqs, f_psd, db)


def fit_exp_step(t_s: np.ndarray, y_db: np.ndarray) -> Tuple[float, float, float]:
    """Fit g(t) = g_final + (g_start - g_final) * exp(-t/tau). Returns (tau_ms, t10_90_ms, g_final)."""
    g_start0 = float(y_db[0])
    g_final0 = float(np.median(y_db[-max(5, len(y_db) // 5):]))

    def res(p):
        g_s, g_f, tau = p
        return (g_f + (g_s - g_f) * np.exp(-t_s / max(tau, 1e-4))) - y_db

    sol = least_squares(res, [g_start0, g_final0, 0.05], bounds=([-120, -120, 0.001], [20, 20, 5.0]))
    tau_s = float(sol.x[2])
    tau_ms = tau_s * 1000.0
    t10_90_ms = tau_s * np.log(9.0) * 1000.0
    return tau_ms, t10_90_ms, float(sol.x[1])


def analyze_drc_burst(burst_rec: np.ndarray, burst_meta: dict, fs: int) -> Tuple[dict, np.ndarray, np.ndarray]:
    """Bandpass around 60 Hz, compute Hilbert envelope in dB, fit attack & release tau."""
    sos = signal.butter(4, [45.0, 75.0], btype="bandpass", fs=fs, output="sos")
    bp = signal.sosfiltfilt(sos, burst_rec)
    analytic = signal.hilbert(bp)
    env_db = 20.0 * np.log10(np.maximum(np.abs(analytic), 1e-9))

    # Align burst recording by finding the first loud burst rise
    hi_thr = float(np.percentile(env_db, 95)) - 6.0
    above = np.where(env_db > hi_thr)[0]
    first_rise = int(above[0]) if len(above) > 0 else burst_meta["bursts"][0]["step_up_sample"]
    offset = first_rise - burst_meta["bursts"][0]["step_up_sample"]

    atk_taus, atk_1090s, rel_taus, rel_1090s = [], [], [], []
    for b in burst_meta["bursts"]:
        u = b["step_up_sample"] + offset
        d = b["step_down_sample"] + offset

        # Attack window: from overshoot peak (within 150 ms after step up) to steady state before step down
        u_search_end = min(len(env_db), u + int(0.15 * fs))
        u_end = min(len(env_db), d - int(0.02 * fs))
        if u >= 0 and u_search_end > u + 10:
            u_pk = u + int(np.argmax(env_db[u:u_search_end]))
            if u_end - u_pk > 100:
                t_u = np.arange(u_end - u_pk) / fs
                tau_ms, t1090, _ = fit_exp_step(t_u, env_db[u_pk:u_end])
                atk_taus.append(tau_ms)
                atk_1090s.append(t1090)

        # Release window: from undershoot dip (30..250 ms after step down) through 1.8 s recovery
        d_s0 = min(len(env_db), d + int(0.03 * fs))
        d_s1 = min(len(env_db), d + int(0.25 * fs))
        d_end = min(len(env_db), d + int(1.9 * fs))
        if d >= 0 and d_s1 > d_s0 + 10:
            d_dip = d_s0 + int(np.argmin(env_db[d_s0:d_s1]))
            if d_end - d_dip > 200:
                t_d = np.arange(d_end - d_dip) / fs
                tau_ms, t1090, _ = fit_exp_step(t_d, env_db[d_dip:d_end])
                rel_taus.append(tau_ms)
                rel_1090s.append(t1090)

    out = {
        "attack_tau_ms_mean": float(np.mean(atk_taus)) if atk_taus else 15.0,
        "attack_tau_ms_std": float(np.std(atk_taus)) if atk_taus else 0.0,
        "attack_10_90_ms_mean": float(np.mean(atk_1090s)) if atk_1090s else 33.0,
        "release_tau_ms_mean": float(np.mean(rel_taus)) if rel_taus else 250.0,
        "release_tau_ms_std": float(np.std(rel_taus)) if rel_taus else 0.0,
        "release_10_90_ms_mean": float(np.mean(rel_1090s)) if rel_1090s else 550.0,
    }
    t_axis = np.arange(len(env_db)) / fs
    return out, t_axis, env_db


def fit_drc_static_curve(levels_dbfs: np.ndarray, gr_bass_db: np.ndarray) -> dict:
    """Fit soft-knee compressor curve to measured low-frequency gain reduction GR(L)."""
    # gr_bass_db is negative (e.g., 0, 0, -0.8, -3.2, -5.5, -8.1)
    def soft_knee_gr(L: np.ndarray, thr: float, ratio: float, knee: float) -> np.ndarray:
        over = L - thr
        half_k = 0.5 * knee
        slope = (1.0 / ratio) - 1.0
        gr = np.zeros_like(L, dtype=np.float64)
        mid = (over > -half_k) & (over < half_k)
        gr[mid] = slope * ((over[mid] + half_k) ** 2) / (2.0 * knee)
        hi = over >= half_k
        gr[hi] = slope * over[hi]
        return gr

    def res(p):
        return soft_knee_gr(levels_dbfs, p[0], p[1], p[2]) - gr_bass_db

    sol = least_squares(res, [-14.0, 4.0, 6.0], bounds=([-30.0, 1.05, 0.5], [0.0, 20.0, 18.0]))
    thr, ratio, knee = [float(v) for v in sol.x]
    sim = soft_knee_gr(levels_dbfs, thr, ratio, knee)
    rmse = float(np.sqrt(np.mean((sim - gr_bass_db) ** 2)))
    return {
        "threshold_s9_dbfs": thr,
        "ratio": ratio,
        "knee_db": knee,
        "fit_rmse_db": rmse,
        "measured_gr_db": gr_bass_db.tolist(),
        "simulated_gr_db": sim.tolist(),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze Studio 9 vs Modified Speaker SysID recordings.")
    parser.add_argument("--meta", type=Path, required=True, help="Path to signals_meta.json")
    parser.add_argument("--s9-stepped", type=Path, required=True, help="Studio 9 stepped sweep WAV")
    parser.add_argument("--s9-burst", type=Path, required=True, help="Studio 9 60 Hz DRC burst WAV")
    parser.add_argument("--m6-takes", type=Path, nargs="+", required=True, help="Modified speaker -20 dBFS sweep WAVs")
    parser.add_argument("--s9-noise", type=Path, default=None, help="Studio 9 room noise WAV")
    parser.add_argument("--m6-noise", type=Path, default=None, help="Modified speaker room noise WAV")
    parser.add_argument("--s9-nf", type=Path, default=None, help="Optional Studio 9 near-field bass sweep WAV")
    parser.add_argument("--m6-nf", type=Path, default=None, help="Optional Modified speaker near-field bass sweep WAV")
    parser.add_argument("--validate", type=Path, default=None, help="Optional post-tuning validation sweep WAV")
    parser.add_argument("--gate-ms", type=float, default=6.0, help="Impulse gate length in ms (default 6.0)")
    parser.add_argument("--smooth", type=float, default=3.0, help="Fractional-octave smoothing N for 1/N-oct (default 3.0)")
    parser.add_argument("--out-dir", type=Path, default=Path("tools/results"), help="Output directory")
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    with open(args.meta, "r", encoding="utf-8") as f:
        meta = json.load(f)

    fs = int(meta["sample_rate"])
    sp = meta["sweep_params"]
    sw, _, L = make_farina_sweep_and_inv(fs, sp["f1_hz"], sp["f2_hz"], sp["duration_s"], sp["fade_ms"])

    grid_freqs = np.geomspace(20.0, 20000.0, 512)
    mid_band = (grid_freqs >= 600.0) & (grid_freqs <= 2500.0)
    center_band = (grid_freqs >= 80.0) & (grid_freqs <= 8000.0)

    # --- 1. Analyze Studio 9 stepped sweeps ---
    s9_rec = load_mono_48k(args.s9_stepped, fs)
    stepped_sweeps = meta["files"]["ess_stepped"]["sweeps"]
    exp_starts = [s["start_sample"] for s in stepped_sweeps]
    found_starts, ppm = locate_sweeps(s9_rec, sw, exp_starts, fs)
    if abs(ppm) > 5.0:
        print(f"[INFO] Studio 9 clock drift {ppm:+.1f} ppm -> resampling...")
        s9_rec = correct_clock_drift(s9_rec, ppm)
        found_starts, ppm = locate_sweeps(s9_rec, sw, exp_starts, fs)

    s9_abs_curves: Dict[float, np.ndarray] = {}
    s9_norm_curves: Dict[float, np.ndarray] = {}
    s9_drc_curves: Dict[float, np.ndarray] = {}
    s9_hd2: Dict[float, float] = {}
    s9_hd3: Dict[float, float] = {}

    for s_info, st in zip(stepped_sweeps, found_starts):
        lvl = float(s_info["level_dbfs"])
        seg = s9_rec[st : st + len(sw) + int(0.5 * fs)]
        ir = deconvolve_regularized(seg, sw, fs, sp["f1_hz"], sp["f2_hz"])
        gated, pk = gate_ir(ir, fs, gate_ms=args.gate_ms)
        pk_ms = 1000.0 * (pk - len(ir) // 2) / fs
        if abs(pk_ms) > 20.0:
            print(f"[WARN] S9 {lvl:+.0f} dBFS IR peak offset {pk_ms:+.1f} ms — check sweep timing/dropouts!")
        resp_abs = ir_to_response_db(gated, fs, grid_freqs, smooth_frac=args.smooth)
        s9_abs_curves[lvl] = resp_abs
        s9_norm_curves[lvl] = resp_abs - float(np.mean(resp_abs[mid_band]))

        # Longer 35 ms gate for low-frequency (50..250 Hz) DRC gain-reduction tracking
        gated_lf, _ = gate_ir(ir, fs, gate_ms=35.0, pre_ms=2.0)
        resp_lf = ir_to_response_db(gated_lf, fs, grid_freqs, smooth_frac=args.smooth)
        s9_drc_curves[lvl] = resp_lf - lvl  # true absolute sensitivity per 0 dBFS drive

        s9_hd2[lvl] = measure_harmonic_ratio_db(ir, pk, L, fs, 2)
        s9_hd3[lvl] = measure_harmonic_ratio_db(ir, pk, L, fs, 3)

    # Linear low-level reference T9 = average of -24 and -18 dBFS normalized curves
    t9_norm = 0.5 * (s9_norm_curves[-24.0] + s9_norm_curves[-18.0])
    s9_sens_1k = float(np.mean(s9_abs_curves[-24.0][mid_band]) - (-24.0))

    # --- 2. Analyze Modified Speaker P6 takes ---
    p6_abs_list = []
    p6_norm_list = []
    for take_path in args.m6_takes:
        m6_rec = load_mono_48k(take_path, fs)
        single_exp = [meta["files"]["ess_single_m20db"]["sweeps"][0]["start_sample"]]
        m6_starts, _ = locate_sweeps(m6_rec, sw, single_exp, fs)
        st = m6_starts[0]
        seg = m6_rec[st : st + len(sw) + int(0.5 * fs)]
        ir = deconvolve_regularized(seg, sw, fs, sp["f1_hz"], sp["f2_hz"])
        gated, pk = gate_ir(ir, fs, gate_ms=args.gate_ms)
        pk_ms = 1000.0 * (pk - len(ir) // 2) / fs
        if abs(pk_ms) > 20.0:
            print(f"[WARN] {take_path.name}: IR peak offset {pk_ms:+.1f} ms — possible sweep underrun/timing mismatch!")
        resp_abs = ir_to_response_db(gated, fs, grid_freqs, smooth_frac=args.smooth)
        p6_abs_list.append(resp_abs)
        p6_norm_list.append(resp_abs - float(np.mean(resp_abs[mid_band])))

    p6_abs = np.mean(p6_abs_list, axis=0)
    p6_norm = np.mean(p6_norm_list, axis=0)
    p6_sens_1k = float(np.mean(p6_abs[mid_band]) - (-20.0))

    # --- 3. Optional near-field bass splice ---
    if args.s9_nf and args.m6_nf:
        for label, nf_path in [("s9", args.s9_nf), ("m6", args.m6_nf)]:
            nf_rec = load_mono_48k(nf_path, fs)
            nf_starts, _ = locate_sweeps(nf_rec, sw, [int(2.0 * fs)], fs)
            seg = nf_rec[nf_starts[0] : nf_starts[0] + len(sw) + int(0.5 * fs)]
            ir = deconvolve_regularized(seg, sw, fs, sp["f1_hz"], sp["f2_hz"])
            gated_nf, pk = gate_ir(ir, fs, gate_ms=30.0, pre_ms=2.0)  # longer gate for NF bass
            pk_ms = 1000.0 * (pk - len(ir) // 2) / fs
            if abs(pk_ms) > 20.0:
                print(f"[WARN] {nf_path.name}: IR peak offset {pk_ms:+.1f} ms — possible sweep underrun/timing mismatch!")
            nf_db = ir_to_response_db(gated_nf, fs, grid_freqs, smooth_frac=args.smooth)
            if label == "s9":
                t9_norm = splice_nearfield(grid_freqs, t9_norm, nf_db)
            else:
                p6_norm = splice_nearfield(grid_freqs, p6_norm, nf_db)

    # Center T9 and P6 to 0 dB broadband mean across 80 Hz..8 kHz so H_req is balanced around 0 dB
    t9_norm -= float(np.mean(t9_norm[center_band]))
    p6_norm -= float(np.mean(p6_norm[center_band]))

    # Required EQ ratio H_req(f) = T9 / P6 (in dB: T9_dB - P6_dB)
    h_req_db = t9_norm - p6_norm

    # --- 4. Noise-floor SNR mask ---
    s9_noise = load_mono_48k(args.s9_noise, fs) if args.s9_noise else None
    m6_noise = load_mono_48k(args.m6_noise, fs) if args.m6_noise else None
    noise_s9_db = compute_noise_psd_db(s9_noise, fs, grid_freqs)
    noise_m6_db = compute_noise_psd_db(m6_noise, fs, grid_freqs)
    snr_valid = ((s9_abs_curves[-24.0] - noise_s9_db) > 10.0) & ((p6_abs - noise_m6_db) > 10.0)
    if args.s9_noise is None and args.m6_noise is None:
        snr_valid[:] = True

    # --- 5. Extract Studio 9 Dynamic Bass Compression (C4) ---
    levels = np.array(sorted(s9_drc_curves.keys()), dtype=np.float64)
    gr_matrix = np.array([s9_drc_curves[lvl] - s9_drc_curves[-24.0] for lvl in levels])
    bass_mask = (grid_freqs >= 55.0) & (grid_freqs <= 85.0)
    gr_bass = np.array([float(np.mean(gr_matrix[i, bass_mask])) for i in range(len(levels))])
    drc_static = fit_drc_static_curve(levels, gr_bass)

    # Estimate sidechain crossover frequency where max-level GR falls to half its bass maximum
    max_gr_curve = gr_matrix[-1]
    peak_gr = float(np.min(max_gr_curve[bass_mask]))
    sidechain_hz = 120.0
    if peak_gr < -1.0:
        above_bass = np.where((grid_freqs > 70.0) & (max_gr_curve > 0.5 * peak_gr))[0]
        if len(above_bass) > 0:
            sidechain_hz = float(grid_freqs[above_bass[0]])

    # Translate Studio 9 threshold to acoustic recorded dBFS and ESP32 digital dBFS
    thr_acoustic = drc_static["threshold_s9_dbfs"] + s9_sens_1k
    thr_esp32 = thr_acoustic - p6_sens_1k

    # 60 Hz burst Hilbert attack/release
    burst_rec = load_mono_48k(args.s9_burst, fs)
    burst_stats, t_burst, env_burst_db = analyze_drc_burst(burst_rec, meta["files"]["drc_burst_60hz"], fs)

    results = {
        "sample_rate": fs,
        "gate_ms": args.gate_ms,
        "clock_drift_ppm": ppm,
        "s9_sensitivity_1k_db": s9_sens_1k,
        "p6_sensitivity_1k_db": p6_sens_1k,
        "level_diff_1k_db": s9_sens_1k - p6_sens_1k,
        "grid_freqs_hz": grid_freqs.tolist(),
        "t9_norm_db": t9_norm.tolist(),
        "p6_norm_db": p6_norm.tolist(),
        "h_req_db": h_req_db.tolist(),
        "snr_valid": snr_valid.tolist(),
        "s9_hd2_db": {str(k): v for k, v in s9_hd2.items()},
        "s9_hd3_db": {str(k): v for k, v in s9_hd3.items()},
        "drc": {
            **drc_static,
            **burst_stats,
            "sidechain_hz": sidechain_hz,
            "threshold_acoustic_dbfs": float(thr_acoustic),
            "threshold_esp32_dbfs": float(thr_esp32),
        },
    }

    # --- 6. Optional post-tuning validation ---
    if args.validate:
        val_rec = load_mono_48k(args.validate, fs)
        v_starts, _ = locate_sweeps(val_rec, sw, [meta["files"]["ess_single_m20db"]["sweeps"][0]["start_sample"]], fs)
        seg = val_rec[v_starts[0] : v_starts[0] + len(sw) + int(0.5 * fs)]
        ir = deconvolve_regularized(seg, sw, fs, sp["f1_hz"], sp["f2_hz"])
        gated, _ = gate_ir(ir, fs, gate_ms=args.gate_ms)
        val_db = ir_to_response_db(gated, fs, grid_freqs, smooth_frac=args.smooth)
        val_norm = val_db - float(np.mean(val_db[center_band]))
        eval_band = (grid_freqs >= 100.0) & (grid_freqs <= 10000.0)
        err = val_norm[eval_band] - t9_norm[eval_band]
        results["validation"] = {
            "val_norm_db": val_norm.tolist(),
            "rms_error_100_10k_db": float(np.sqrt(np.mean(err ** 2))),
            "max_error_100_10k_db": float(np.max(np.abs(err))),
        }
        plt.figure(figsize=(10, 5))
        plt.semilogx(grid_freqs, t9_norm, label="Studio 9 (T9)")
        plt.semilogx(grid_freqs, val_norm, label="Tuned Modified Speaker")
        plt.semilogx(grid_freqs, val_norm - t9_norm, "--", label="Residual (Tuned - T9)")
        plt.xlim(20, 20000); plt.ylim(-25, 15); plt.grid(True, which="both", alpha=0.3)
        plt.xlabel("Frequency (Hz)"); plt.ylabel("Level (dB, norm @ 1 kHz)")
        plt.title(f"Validation: RMS err (100Hz-10kHz) = {results['validation']['rms_error_100_10k_db']:.2f} dB")
        plt.legend(); plt.tight_layout()
        plt.savefig(args.out_dir / "plot_validation.png", dpi=150); plt.close()

    with open(args.out_dir / "sysid_results.json", "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)

    # --- Plots ---
    plt.figure(figsize=(10, 6))
    plt.semilogx(grid_freqs, t9_norm, label="Studio 9 T9 (low-level ref)")
    plt.semilogx(grid_freqs, p6_norm, label="Modified Speaker P6 (flat)")
    plt.semilogx(grid_freqs, h_req_db, label="Required EQ H_req = T9 / P6", linewidth=2)
    plt.xlim(20, 20000); plt.ylim(-25, 20); plt.grid(True, which="both", alpha=0.3)
    plt.xlabel("Frequency (Hz)"); plt.ylabel("dB (normalized @ 1 kHz)")
    plt.title("Measured Frequency Responses & Required EQ")
    plt.legend(); plt.tight_layout()
    plt.savefig(args.out_dir / "plot_responses.png", dpi=150); plt.close()

    plt.figure(figsize=(10, 5))
    for i, lvl in enumerate(levels):
        plt.semilogx(grid_freqs, gr_matrix[i], label=f"{lvl:+.0f} dBFS")
    plt.xlim(20, 2000); plt.ylim(-18, 4); plt.grid(True, which="both", alpha=0.3)
    plt.xlabel("Frequency (Hz)"); plt.ylabel("Gain Reduction vs -24 dBFS (dB)")
    plt.title(f"Studio 9 Dynamic Bass Compression (sidechain ~{sidechain_hz:.0f} Hz, thr={thr_esp32:.1f} dBFS ESP32)")
    plt.legend(); plt.tight_layout()
    plt.savefig(args.out_dir / "plot_drc_gr.png", dpi=150); plt.close()

    plt.figure(figsize=(10, 4))
    plt.plot(t_burst, env_burst_db, linewidth=0.8)
    plt.grid(True, alpha=0.3); plt.xlabel("Time (s)"); plt.ylabel("60 Hz Envelope (dB)")
    plt.title(
        f"60 Hz DRC Burst Envelope — Attack tau={burst_stats['attack_tau_ms_mean']:.1f} ms, "
        f"Release tau={burst_stats['release_tau_ms_mean']:.1f} ms"
    )
    plt.tight_layout()
    plt.savefig(args.out_dir / "plot_drc_burst.png", dpi=150); plt.close()

    print(f"Saved analysis results and plots to {args.out_dir}/")
    print(f"  Sensitivity @ 1 kHz: S9={s9_sens_1k:.1f} dB, P6={p6_sens_1k:.1f} dB")
    print(f"  DRC: sidechain={sidechain_hz:.1f} Hz, thr_esp32={thr_esp32:.1f} dBFS, "
          f"ratio={drc_static['ratio']:.2f}:1, knee={drc_static['knee_db']:.1f} dB, "
          f"atk={burst_stats['attack_tau_ms_mean']:.1f} ms, rel={burst_stats['release_tau_ms_mean']:.1f} ms")


if __name__ == "__main__":
    main()
