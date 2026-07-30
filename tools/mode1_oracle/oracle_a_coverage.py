"""Does the package cover every part of the song the original actually sings?

Oracle A showed the micro clips rebuild the converted vocal wherever they
exist. This checks the complementary failure: stretches where the original
vocal is clearly singing but no micro phrase is scheduled, i.e. lyrics that can
never be played back no matter how good the follower is.

Usage:
  python oracle_a_coverage.py <song_package.json> <output-dir>
"""
from __future__ import annotations

import csv
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, load_package, read_audio, rms_envelope

MIN_GAP_SEC = 0.35     # ignore ordinary breath-sized rests
VOICED_FRAC = 0.20     # "singing" = above this fraction of the track's typical level


def main(package_path: str, out_dir: str):
    pkg = load_package(package_path)
    micro = pkg["micro_phrases"]

    sep, _ = read_audio(pkg["audio"]["source_vocal"])
    t, r = rms_envelope(sep, SR, win_ms=30.0, hop_ms=10.0)

    active = r[r > np.percentile(r, 40)]
    level = float(np.median(active))
    thr = level * VOICED_FRAC

    first = float(micro[0]["score"]["start_sec"])
    last = float(micro[-1]["score"]["end_sec"])

    gaps = []
    for a, b in zip(micro, micro[1:]):
        g0, g1 = float(a["score"]["end_sec"]), float(b["score"]["start_sec"])
        if g1 - g0 >= MIN_GAP_SEC:
            gaps.append((g0, g1, a["phrase_id"], b["phrase_id"]))

    rows = []
    for g0, g1, pa, pb in gaps:
        lo, hi = np.searchsorted(t, g0), np.searchsorted(t, g1)
        if hi <= lo:
            continue
        seg = r[lo:hi]
        voiced = float(np.mean(seg >= thr))
        rows.append({
            "gap_start_sec": round(g0, 3),
            "gap_end_sec": round(g1, 3),
            "gap_duration_sec": round(g1 - g0, 3),
            "after_phrase": pa,
            "before_phrase": pb,
            "voiced_fraction": round(voiced, 3),
            "voiced_seconds": round(voiced * (g1 - g0), 3),
            "peak_rel_to_level": round(float(np.max(seg)) / level, 3),
            "verdict": "MISSING_LYRICS" if voiced > 0.35 else
                       ("partial" if voiced > 0.10 else "true_rest"),
        })

    rows.sort(key=lambda x: -x["voiced_seconds"])
    path = os.path.join(out_dir, "coverage_gaps.csv")
    os.makedirs(out_dir, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # also: vocal energy before the first phrase and after the last
    def frac(a, b):
        lo, hi = np.searchsorted(t, a), np.searchsorted(t, b)
        if hi <= lo:
            return 0.0, 0.0
        seg = r[lo:hi]
        v = float(np.mean(seg >= thr))
        return round(v, 3), round(v * (b - a), 3)

    total_song = len(sep) / SR
    print("package covers %.2f .. %.2f s of a %.2f s track" % (first, last, total_song))
    print("gaps >= %.2fs: %d" % (MIN_GAP_SEC, len(rows)))
    miss = [r for r in rows if r["verdict"] == "MISSING_LYRICS"]
    part = [r for r in rows if r["verdict"] == "partial"]
    print("  MISSING_LYRICS: %d  (%.2f s of singing with no phrase)"
          % (len(miss), sum(r["voiced_seconds"] for r in miss)))
    print("  partial       : %d  (%.2f s)"
          % (len(part), sum(r["voiced_seconds"] for r in part)))
    print("  true_rest     : %d" % sum(1 for r in rows if r["verdict"] == "true_rest"))
    print("before first phrase 0..%.2f : voiced_frac=%s voiced_sec=%s" % ((first,) + frac(0.0, first)))
    print("after last phrase %.2f..end : voiced_frac=%s voiced_sec=%s" % ((last,) + frac(last, total_song)))
    print()
    print("worst gaps:")
    for r in rows[:10]:
        print("  %7.2f-%7.2f (%5.2fs) voiced=%4.0f%% -> %-14s after %s"
              % (r["gap_start_sec"], r["gap_end_sec"], r["gap_duration_sec"],
                 r["voiced_fraction"] * 100, r["verdict"], r["after_phrase"]))
    print("wrote", path)
    return rows


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
