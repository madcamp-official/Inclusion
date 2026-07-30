"""Separate "the map is late" from "the consonant was cut off".

Oracle B has no follower in it, so when the vocal still feels slightly late the
cause is either the timing map or the asset. Oracle C measured the asset side:
the sung attack begins ~150 ms before the scored vowel, but the micro clips
carry only 80 ms of lead-in, so the consonant is clipped and the ear hears the
vowel arriving bare.

This renders three variants against the same guitar so the two explanations can
be told apart by listening:

  v1_pad080_nudge000   what Oracle B already does (80 ms lead-in)
  v2_pad250_nudge000   re-cut with 250 ms lead-in: the whole consonant is there,
                       the vowel still lands on the mapped beat
  v3_pad080_nudge100   Oracle B pulled 100 ms earlier: no extra consonant, the
                       whole vocal simply arrives sooner

If v2 fixes it, it is the asset. If v3 fixes it, it is the map. If only v3 does,
the lead-in is not the issue after all.

Clips are cut from the original-key vocal, so no pitch shifting is involved and
no shifter artefact can colour the judgement.

Usage:
  python oracle_b_leadin_test.py <original_key_package.json> <guitar.wav>
                                 <piecewise_map.json> <out-dir>
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, load_package, read_audio, write_audio
from piecewise_map import map_score_to_performance

VARIANTS = [
    ("v1_pad080_nudge000", 0.080, 0.000),
    ("v2_pad250_nudge000", 0.250, 0.000),
    ("v3_pad080_nudge100", 0.080, 0.100),
    ("v4_pad080_nudge200", 0.080, 0.200),
    ("v5_pad250_nudge100", 0.250, 0.100),
    ("v6_pad250_nudge200", 0.250, 0.200),
]


def render(full, micro, pw, total, pad, nudge):
    out = np.zeros(total)
    weight_spans = []
    for m in micro:
        s0 = float(m["score"]["start_sec"])
        s1 = float(m["score"]["end_sec"])
        p = float(map_score_to_performance(pw, s0)) - nudge
        a_src = int(round((s0 - pad) * SR))
        b_src = int(round((s1 + 0.080) * SR))
        if a_src < 0:
            a_src = 0
        seg = full[a_src:b_src]
        if seg.size == 0:
            continue
        start = int(round((p - pad) * SR))
        weight_spans.append((start, start + len(seg), seg))

    for i, (s, e, seg) in enumerate(weight_spans):
        ramp = np.ones(len(seg))
        if i > 0:
            ov = weight_spans[i - 1][1] - s
            if ov > 1:
                n = min(ov, len(seg))
                ramp[:n] *= np.linspace(0.0, 1.0, n, endpoint=False)
        if i + 1 < len(weight_spans):
            ov = e - weight_spans[i + 1][0]
            if ov > 1:
                n = min(ov, len(seg))
                ramp[len(seg) - n:] *= np.linspace(1.0, 0.0, n, endpoint=False)
        a, b = max(0, s), min(total, e)
        if b > a:
            out[a:b] += (seg * ramp)[a - s:b - s]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("guitar")
    ap.add_argument("piecewise_map")
    ap.add_argument("out_dir")
    ap.add_argument("--guitar-gain", type=float, default=1.0)
    args = ap.parse_args()

    pkg = load_package(args.package)
    micro = pkg["micro_phrases"]
    full, _ = read_audio(pkg["audio"]["converted_vocal"])
    full = full.reshape(-1)
    gtr, _ = read_audio(args.guitar)
    gtr = gtr.reshape(-1)
    total = len(gtr)
    with open(args.piecewise_map, encoding="utf-8") as f:
        pw = json.load(f)

    os.makedirs(args.out_dir, exist_ok=True)
    manifest = []
    for name, pad, nudge in VARIANTS:
        print("rendering %s (lead-in %.0f ms, nudge -%.0f ms)"
              % (name, pad * 1000, nudge * 1000))
        v = render(full, micro, pw, total, pad, nudge)
        write_audio(os.path.join(args.out_dir, name + "_vocal.wav"), v)
        write_audio(os.path.join(args.out_dir, name + "_guitar_plus_vocal.wav"),
                    args.guitar_gain * gtr + v)
        manifest.append({"variant": name, "lead_in_sec": pad,
                         "nudge_earlier_sec": nudge,
                         "first_vocal_performance_sec": round(
                             float(map_score_to_performance(pw, 9.952)) - nudge, 4)})

    with open(os.path.join(args.out_dir, "variants.json"), "w",
              encoding="utf-8") as f:
        json.dump({"guitar": os.path.abspath(args.guitar),
                   "package": os.path.abspath(args.package),
                   "note": ("all three place the scored vowel at the same mapped "
                            "time except v3, which is uniformly 100 ms earlier"),
                   "variants": manifest}, f, ensure_ascii=False, indent=2)
    print("wrote", args.out_dir)


if __name__ == "__main__":
    main()
