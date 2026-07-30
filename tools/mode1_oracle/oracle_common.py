"""Shared helpers for the Mode 1 Oracle A/B/C renders.

Read-only with respect to every user-supplied asset: sources are opened, never
written back. All generated audio goes to output/mode1_oracle/.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

SR = 48000


# --------------------------------------------------------------------------
# io
# --------------------------------------------------------------------------
def read_audio(path: str, target_sr: int = SR, mono: bool = True):
    """Read any format, optionally downmix and resample to target_sr."""
    x, sr = sf.read(path, dtype="float64", always_2d=True)
    if mono:
        x = x.mean(axis=1, keepdims=True)
    if sr != target_sr:
        from math import gcd

        g = gcd(int(sr), int(target_sr))
        x = resample_poly(x, target_sr // g, sr // g, axis=0)
    return np.ascontiguousarray(x), target_sr


def write_audio(path: str, x: np.ndarray, sr: int = SR):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if x.ndim == 1:
        x = x[:, None]
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    if peak > 0.999:
        x = x * (0.999 / peak)
    sf.write(path, x, sr, subtype="PCM_24")
    return path


def load_package(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------
def rms_envelope(x: np.ndarray, sr: int = SR, win_ms: float = 10.0,
                 hop_ms: float = 1.0):
    """Frame-wise RMS. Returns (times_sec, rms)."""
    x = x.reshape(-1)
    win = max(1, int(sr * win_ms / 1000.0))
    hop = max(1, int(sr * hop_ms / 1000.0))
    if len(x) < win:
        x = np.pad(x, (0, win - len(x)))
    n = 1 + (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    frames = x[idx]
    r = np.sqrt(np.mean(frames * frames, axis=1))
    t = (np.arange(n) * hop + win / 2.0) / sr
    return t, r


def audible_onset(x: np.ndarray, sr: int = SR, rel_db: float = -40.0,
                  abs_floor: float = 3e-4) -> float | None:
    """First time the RMS envelope crosses a level-relative threshold.

    Returns seconds from the start of x, or None when the clip never rises
    above the floor.
    """
    t, r = rms_envelope(x, sr)
    if r.size == 0:
        return None
    ref = float(np.percentile(r, 95))
    thr = max(ref * (10.0 ** (rel_db / 20.0)), abs_floor)
    hits = np.flatnonzero(r >= thr)
    if hits.size == 0:
        return None
    return float(t[hits[0]])


def onset_strength(x: np.ndarray, sr: int = SR, hop_ms: float = 2.0):
    """Spectral-flux onset strength envelope. Returns (times_sec, flux)."""
    x = x.reshape(-1)
    n_fft = 1024
    hop = max(1, int(sr * hop_ms / 1000.0))
    if len(x) < n_fft:
        x = np.pad(x, (0, n_fft - len(x)))
    n = 1 + (len(x) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(n)[:, None]
    w = np.hanning(n_fft)[None, :]
    spec = np.abs(np.fft.rfft(frames_view(x, idx) * w, axis=1))
    logspec = np.log1p(spec * 100.0)
    flux = np.diff(logspec, axis=0)
    flux = np.maximum(flux, 0.0).sum(axis=1)
    flux = np.concatenate([[0.0], flux])
    t = (np.arange(n) * hop + n_fft / 2.0) / sr
    return t, flux


def frames_view(x, idx):
    return x[idx]


def pick_onsets(t, flux, min_sep_sec=0.06, rel_thresh=0.30):
    """Peak-pick an onset envelope with a moving-median floor."""
    if flux.size == 0:
        return np.array([])
    k = 101
    pad = k // 2
    padded = np.pad(flux, (pad, pad), mode="edge")
    local = np.lib.stride_tricks.sliding_window_view(padded, k)
    floor = np.median(local, axis=1)
    scaled = flux - floor
    hi = float(np.percentile(scaled, 99.5))
    if hi <= 0:
        return np.array([])
    thr = hi * rel_thresh
    cand = []
    last = -1e9
    for i in range(1, len(scaled) - 1):
        if scaled[i] < thr:
            continue
        if scaled[i] < scaled[i - 1] or scaled[i] < scaled[i + 1]:
            continue
        if t[i] - last < min_sep_sec:
            continue
        cand.append(t[i])
        last = t[i]
    return np.array(cand)


# --------------------------------------------------------------------------
# reconstruction
# --------------------------------------------------------------------------
@dataclass
class Placement:
    phrase_id: str
    index: int
    clip_path: str
    target_sample: int      # where sample 0 of the clip lands in the output
    score_start_sec: float
    score_end_sec: float


def overlap_add(placements, clips, total_samples: int,
                crossfade: bool = True) -> np.ndarray:
    """Place clips on a timeline.

    Every micro clip is cut from the same converted vocal, so overlapping
    regions hold identical audio; a *linear* crossfade therefore reconstructs
    the source exactly. With crossfade=False clips are summed, which doubles
    the overlaps -- that variant exists to expose the overlap explicitly.
    """
    out = np.zeros(total_samples, dtype=np.float64)
    if not crossfade:
        for p, c in zip(placements, clips):
            s = p.target_sample
            seg = c.reshape(-1)
            a, b = max(0, s), min(total_samples, s + len(seg))
            if b > a:
                out[a:b] += seg[a - s:b - s]
        return out

    weight = np.zeros(total_samples, dtype=np.float64)
    spans = []
    for p, c in zip(placements, clips):
        s = p.target_sample
        seg = c.reshape(-1).copy()
        spans.append((s, s + len(seg), seg))

    for i, (s, e, seg) in enumerate(spans):
        ramp = np.ones(len(seg), dtype=np.float64)
        if i > 0:
            ov = spans[i - 1][1] - s              # overlap with previous clip
            if ov > 1:
                n = min(ov, len(seg))
                ramp[:n] *= np.linspace(0.0, 1.0, n, endpoint=False)
        if i + 1 < len(spans):
            ov = e - spans[i + 1][0]              # overlap with next clip
            if ov > 1:
                n = min(ov, len(seg))
                ramp[len(seg) - n:] *= np.linspace(1.0, 0.0, n, endpoint=False)
        a, b = max(0, s), min(total_samples, e)
        if b > a:
            out[a:b] += (seg * ramp)[a - s:b - s]
            weight[a:b] += ramp[a - s:b - s]
    return out


def db(x: float) -> float:
    return -np.inf if x <= 0 else 20.0 * np.log10(x)
