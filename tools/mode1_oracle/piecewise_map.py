"""Build a monotonic piecewise score<->performance map from chroma evidence.

A single affine map cannot describe these takes: fitting the guitar in
overlapping windows gives locally-best tempos of 0.94-0.98, i.e. the player
drifts. This walks the take in windows, takes each window's best local
alignment as an anchor, discards anchors whose evidence is weak or which would
make the map run backwards, and interpolates between the survivors.

Usage:
  python piecewise_map.py <song_package.json> <guitar.wav> <out.json>
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, load_package, read_audio
from chroma_align import chroma, chord_pitch_classes

WINDOW = 8.0
HOP = 4.0
MIN_R = 0.08
# A pair of anchors implies a local tempo. Anything outside this band is a
# misaligned window, not playing: below it the score is crammed through the
# performance (lyrics pile up with no rests), above it the score stalls.
MIN_RATE = 0.88
MAX_RATE = 1.15
# Cost per second of performance spent at a tempo away from 1.0. Two weak
# anchors twelve seconds apart can imply a 0.84 rate and still sit inside the
# hard band, which runs the score ~19% fast and makes the lyrics crowd forward.
# Charging for the drift makes a long stretch prefer a near-unity tempo unless
# the evidence at both ends is genuinely strong.
RATE_PENALTY = 0.10


def build(package_path, guitar_path, until=None):
    pkg = load_package(package_path)
    chords = pkg["chord_timeline"]
    starts = np.array([c["start_sec"] for c in chords])
    base = [chord_pitch_classes(c.get("raw_chord")) for c in chords]

    g, _ = read_audio(guitar_path)
    g = g.reshape(-1)
    if until:
        g = g[:int(until * SR)]
    C, hop = chroma(g)
    t = np.arange(len(C)) * hop
    Cz = C - C.mean(1, keepdims=True)
    Cz /= np.maximum(np.linalg.norm(Cz, axis=1, keepdims=True), 1e-9)

    cache = {}

    def template(idx):
        out = np.zeros((len(idx), 12))
        for j, ci in enumerate(idx):
            if ci not in cache:
                v = np.zeros(12)
                pcs = base[ci]
                if pcs:
                    for p in pcs:
                        v[p] = 1.0
                    v /= v.sum()
                cache[ci] = v
            out[j] = cache[ci]
        return out

    def corr(off, scale, lo, hi):
        m = (t >= lo) & (t < hi)
        if m.sum() < 50:
            return -9
        st = (t[m] - off) / scale
        ok = (st >= 0) & (st <= starts[-1])
        if ok.sum() < 50:
            return -9
        idx = np.clip(np.searchsorted(starts, st[ok], side="right") - 1,
                      0, len(chords) - 1)
        T = template(idx)
        Tz = T - T.mean(1, keepdims=True)
        Tz /= np.maximum(np.linalg.norm(Tz, axis=1, keepdims=True), 1e-9)
        return float(np.mean(np.sum(Cz[m][ok] * Tz, axis=1)))

    dur = len(g) / SR
    anchors = []
    lo = 0.0
    while lo + WINDOW <= dur:
        hi = lo + WINDOW
        best = None
        for sc in np.arange(0.93, 1.05, 0.003):
            for off in np.arange(-0.5, 3.0, 0.02):
                r = corr(off, sc, lo, hi)
                if best is None or r > best[0]:
                    best = (r, off, sc)
        r, off, sc = best
        centre = (lo + hi) / 2.0
        anchors.append({
            "performance_sec": round(centre, 4),
            "score_sec": round((centre - off) / sc, 4),
            "window": [round(lo, 2), round(hi, 2)],
            "local_offset": round(off, 4),
            "local_scale": round(sc, 4),
            "correlation": round(r, 4),
        })
        lo += HOP

    kept = [a for a in anchors if a["correlation"] >= MIN_R]
    kept.sort(key=lambda a: a["performance_sec"])
    chain = select_chain(kept)
    return {
        "guitar": os.path.abspath(guitar_path),
        "package": os.path.abspath(package_path),
        "window_sec": WINDOW,
        "hop_sec": HOP,
        "min_correlation": MIN_R,
        "min_rate": MIN_RATE,
        "max_rate": MAX_RATE,
        "all_anchors": anchors,
        "anchors": chain,
        "rejected_anchors": [a for a in anchors if a not in chain],
    }


def select_chain(kept):
    """Pick the highest-evidence anchor chain that never implies a silly tempo.

    Plain monotonicity is not enough: two weakly-correlated windows can sit in
    the right order yet imply the score racing through the performance at 1.8x,
    which crams whole phrases together with no rests between them. Each pair is
    therefore required to imply a local rate inside [MIN_RATE, MAX_RATE], and
    among the chains that satisfy that we keep the one with the most total
    correlation.
    """
    n = len(kept)
    if n == 0:
        return []
    best = [a["correlation"] for a in kept]
    prev = [-1] * n
    for i in range(n):
        for j in range(i):
            dp = kept[i]["performance_sec"] - kept[j]["performance_sec"]
            ds = kept[i]["score_sec"] - kept[j]["score_sec"]
            if ds <= 1e-6 or dp <= 1e-6:
                continue
            rate = dp / ds
            if not (MIN_RATE <= rate <= MAX_RATE):
                continue
            cand = (best[j] + kept[i]["correlation"]
                    - RATE_PENALTY * abs(rate - 1.0) * dp)
            if cand > best[i]:
                best[i] = cand
                prev[i] = j
    end = max(range(n), key=lambda i: best[i])
    out = []
    while end != -1:
        out.append(kept[end])
        end = prev[end]
    return out[::-1]


def map_score_to_performance(doc, score_t):
    a = doc["anchors"]
    if len(a) < 2:
        raise ValueError("not enough anchors")
    xs = np.array([p["score_sec"] for p in a])
    ys = np.array([p["performance_sec"] for p in a])
    s = np.atleast_1d(np.asarray(score_t, dtype=float))
    out = np.interp(s, xs, ys)
    # extrapolate with the end slopes so the intro and outro stay sensible
    if len(xs) >= 2:
        lo_slope = (ys[1] - ys[0]) / max(xs[1] - xs[0], 1e-9)
        hi_slope = (ys[-1] - ys[-2]) / max(xs[-1] - xs[-2], 1e-9)
        out = np.where(s < xs[0], ys[0] + (s - xs[0]) * lo_slope, out)
        out = np.where(s > xs[-1], ys[-1] + (s - xs[-1]) * hi_slope, out)
    return out if np.ndim(score_t) else float(out[0])


def robust_global(doc, min_r=0.20):
    """Replace the chain with one straight line fitted to strong anchors only.

    Mid-song chroma evidence is weak (r ~ 0.10), and a piecewise chain built on
    it chases noise: two weak anchors twenty seconds apart implied a 0.885 rate,
    running the score 13% fast so the lyrics crowd forward. The strong anchors
    agree on a near-constant tempo, so where there is no real evidence it is
    better to keep going at that tempo than to invent a local one.
    """
    strong = [a for a in doc["all_anchors"] if a["correlation"] >= min_r]
    if len(strong) < 2:
        return doc
    x = np.array([a["score_sec"] for a in strong])
    y = np.array([a["performance_sec"] for a in strong])
    w = np.array([a["correlation"] for a in strong])
    A = np.vstack([x, np.ones(len(x))]).T
    sol, *_ = np.linalg.lstsq(A * w[:, None], y * w, rcond=None)
    a_, b_ = float(sol[0]), float(sol[1])
    resid = y - (a_ * x + b_)
    doc["robust_global"] = {
        "rate": round(a_, 5),
        "offset_sec": round(b_, 4),
        "strong_anchor_count": len(strong),
        "min_correlation": min_r,
        "max_abs_residual_ms": round(float(np.max(np.abs(resid))) * 1000, 1),
    }
    lo, hi = float(x.min()), float(x.max())
    doc["anchors"] = [
        {"performance_sec": round(a_ * s + b_, 4), "score_sec": round(s, 4),
         "correlation": None, "source": "robust_global"}
        for s in (0.0, lo, hi, hi + 60.0)
    ]
    return doc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("guitar")
    ap.add_argument("out_json")
    ap.add_argument("--until", type=float, default=None)
    ap.add_argument("--robust-global", action="store_true",
                    help="fit one line to the strong anchors instead of "
                         "chaining every window")
    ap.add_argument("--min-strong-r", type=float, default=0.20)
    args = ap.parse_args()
    doc = build(args.package, args.guitar, args.until)
    if args.robust_global:
        doc = robust_global(doc, args.min_strong_r)
        print("robust global fit:", json.dumps(doc["robust_global"]))
    os.makedirs(os.path.dirname(os.path.abspath(args.out_json)), exist_ok=True)
    with open(args.out_json, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    print("anchors kept %d / %d" % (len(doc["anchors"]), len(doc["all_anchors"])))
    for a in doc["anchors"]:
        if a.get("correlation") is None:
            print("   perf %7.2fs <- score %7.2fs   (%s)"
                  % (a["performance_sec"], a["score_sec"], a.get("source", "-")))
        else:
            print("   perf %7.2fs <- score %7.2fs   r=%.3f  local scale %.4f"
                  % (a["performance_sec"], a["score_sec"], a["correlation"],
                     a["local_scale"]))
    print("first vocal (score 9.952) ->  %.3fs"
          % map_score_to_performance(doc, 9.952))
    print("wrote", args.out_json)


if __name__ == "__main__":
    main()
