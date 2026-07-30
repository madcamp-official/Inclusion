"""Resolve the score<->performance phase by matching chord content, not rhythm.

The intro repeats a two-bar pattern, so onset timing alone cannot tell which
repetition the player is on -- every candidate offset a whole pattern apart
scores the same. Harmony can: Db/F, Gb, Ab and Bbm7 occupy different pitch
classes, so correlating the guitar's chroma against the score's chord sequence
picks the correct repetition outright.

Usage:
  python chroma_align.py <song_package.json> <guitar.wav> [--scale-min .95]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, load_package, read_audio

PITCH = {"C": 0, "C#": 1, "DB": 1, "D": 2, "D#": 3, "EB": 3, "E": 4, "FB": 4,
         "F": 5, "F#": 6, "GB": 6, "G": 7, "G#": 8, "AB": 8, "A": 9,
         "A#": 10, "BB": 10, "B": 11, "CB": 11}


def chord_pitch_classes(label: str):
    """Rough pitch-class set for a chord label such as 'Bbm7' or 'Db/F'."""
    if not label or label in ("N", "-"):
        return None
    bass = None
    if "/" in label:
        label, bass_s = label.split("/", 1)
        bass = PITCH.get(bass_s.strip().upper())
    s = label.strip()
    root_s = s[:2] if len(s) > 1 and s[1] in "#b" else s[:1]
    root = PITCH.get(root_s.upper())
    if root is None:
        return None
    rest = s[len(root_s):].lower()
    minor = rest.startswith("m") and not rest.startswith("maj")
    pcs = {root, (root + (3 if minor else 4)) % 12, (root + 7) % 12}
    if "7" in rest:
        pcs.add((root + (10 if not rest.startswith("maj") else 11)) % 12)
    if "sus4" in rest:
        pcs.discard((root + (3 if minor else 4)) % 12)
        pcs.add((root + 5) % 12)
    if "sus2" in rest:
        pcs.discard((root + (3 if minor else 4)) % 12)
        pcs.add((root + 2) % 12)
    if bass is not None:
        pcs.add(bass)
    return pcs


def chroma(x, sr=SR, hop_ms=20.0, n_fft=8192, fmin=70.0, fmax=2000.0):
    hop = int(sr * hop_ms / 1000.0)
    n = 1 + max(0, (len(x) - n_fft) // hop)
    w = np.hanning(n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    band = (freqs >= fmin) & (freqs <= fmax)
    f = freqs[band]
    pc = np.round(12 * np.log2(np.maximum(f, 1e-9) / 440.0) + 69).astype(int) % 12
    out = np.zeros((n, 12))
    for i in range(n):
        seg = x[i * hop:i * hop + n_fft] * w
        mag = np.abs(np.fft.rfft(seg))[band]
        for k in range(12):
            out[i, k] = mag[pc == k].sum()
    norm = out.sum(axis=1, keepdims=True)
    out = out / np.maximum(norm, 1e-9)
    return out, hop / sr


def score_template(chords, times):
    """12-dim expected chroma at each score time."""
    starts = np.array([c["start_sec"] for c in chords])
    tmpl = np.zeros((len(times), 12))
    idx = np.clip(np.searchsorted(starts, times, side="right") - 1, 0, len(chords) - 1)
    cache = {}
    for i, ci in enumerate(idx):
        if ci not in cache:
            pcs = chord_pitch_classes(chords[ci].get("chord"))
            v = np.zeros(12)
            if pcs:
                for p in pcs:
                    v[p] = 1.0
                v /= v.sum()
            cache[ci] = v
        tmpl[i] = cache[ci]
    return tmpl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("guitar")
    ap.add_argument("--scale-min", type=float, default=0.94)
    ap.add_argument("--scale-max", type=float, default=1.08)
    ap.add_argument("--offset-min", type=float, default=-1.0)
    ap.add_argument("--offset-max", type=float, default=3.5)
    ap.add_argument("--until", type=float, default=48.0)
    args = ap.parse_args()

    pkg = load_package(args.package)
    chords = pkg["chord_timeline"]
    g, _ = read_audio(args.guitar)
    g = g.reshape(-1)[:int(args.until * SR)]

    print("computing chroma ...")
    C, hop = chroma(g)
    t = np.arange(len(C)) * hop
    Cz = C - C.mean(axis=1, keepdims=True)
    Cz /= np.maximum(np.linalg.norm(Cz, axis=1, keepdims=True), 1e-9)

    best = []
    for scale in np.arange(args.scale_min, args.scale_max + 1e-9, 0.002):
        for off in np.arange(args.offset_min, args.offset_max + 1e-9, 0.02):
            score_t = (t - off) / scale
            ok = (score_t >= 0) & (score_t <= chords[-1]["start_sec"])
            if ok.sum() < 100:
                continue
            T = score_template(chords, score_t[ok])
            Tz = T - T.mean(axis=1, keepdims=True)
            Tz /= np.maximum(np.linalg.norm(Tz, axis=1, keepdims=True), 1e-9)
            r = float(np.mean(np.sum(Cz[ok] * Tz, axis=1)))
            best.append((r, float(off), float(scale)))
    best.sort(reverse=True)
    print("\ntop chroma alignments (correlation, offset, scale):")
    for r, off, sc in best[:8]:
        print("   r=%.4f  offset=%+.3fs  scale=%.4f (tempo x%.4f)   "
              "score 0.552 -> %.3fs, score 9.952 -> %.3fs"
              % (r, off, sc, 1.0 / sc, 0.552 * sc + off, 9.952 * sc + off))
    r, off, sc = best[0]
    print("\nBEST: offset=%.4f scale=%.4f" % (off, sc))
    print(json.dumps({"offset_sec": round(off, 4),
                      "scale": round(sc, 4),
                      "correlation": round(r, 4),
                      "first_chord_perf_sec": round(0.552 * sc + off, 4),
                      "first_vocal_perf_sec": round(9.952 * sc + off, 4)}, indent=2))


if __name__ == "__main__":
    main()
