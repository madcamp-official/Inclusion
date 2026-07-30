"""Oracle B -- give the vocal a *correct* performance timing map and nothing else.

No onset follower, no chord follower, no BeatClock. A score->performance affine
map is fitted against the real guitar take, every micro phrase is scheduled at
its mapped sample, and the result is mixed with the guitar. If that sounds
locked, a free-running transport is the right architecture and the current
delay is entirely the follower's doing.

Anchors are proposed by an exhaustive (offset, tempo) search against the score
chord grid rather than guessed, then rendered as spectrograms for visual
verification. They are NOT verified by listening -- see report.md.

Usage:
  python oracle_b.py <song_package.json> <guitar.wav> <output-dir> [--compare run_dir]
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import (SR, load_package, onset_strength, overlap_add,
                           pick_onsets, read_audio, write_audio, Placement)


def detect_onsets(guitar, sr=SR):
    t, flux = onset_strength(guitar, sr, hop_ms=2.0)
    times = pick_onsets(t, flux, min_sep_sec=0.08, rel_thresh=0.22)
    # strength at each pick, for weighting
    strengths = []
    for x in times:
        i = int(np.searchsorted(t, x))
        strengths.append(float(flux[min(i, len(flux) - 1)]))
    s = np.array(strengths)
    if s.size:
        s = s / max(s.max(), 1e-9)
    return times, s


def active_segments(guitar, sr=SR, quiet_sec=0.7):
    """Stretches where the guitar is actually playing.

    Real takes contain long pauses; a single affine map cannot span them, and
    including them is what makes a global fit explode.
    """
    from oracle_common import rms_envelope
    t, r = rms_envelope(guitar, sr, win_ms=50.0, hop_ms=10.0)
    thr = float(np.percentile(r, 70)) * 0.08
    loud = r >= thr
    segs, i = [], 0
    while i < len(loud):
        if loud[i]:
            j = i
            while j < len(loud) and (loud[j] or
                                     (j + int(quiet_sec * 100) < len(loud)
                                      and loud[j:j + int(quiet_sec * 100)].any())):
                j += 1
            segs.append((float(t[i]), float(t[min(j, len(t) - 1)])))
            i = j
        else:
            i += 1
    return [(a, b) for a, b in segs if b - a >= 2.0]


def icp_refine(onsets, score_times, offset, scale, tol=0.080, iters=6):
    """Snap score events to nearby onsets, least-squares refit, repeat."""
    st = np.asarray(score_times, dtype=float)
    for _ in range(iters):
        mapped = st * scale + offset
        idx = np.clip(np.searchsorted(onsets, mapped), 1, len(onsets) - 1)
        left, right = onsets[idx - 1], onsets[idx]
        near = np.where(np.abs(mapped - left) < np.abs(mapped - right),
                        left, right)
        d = np.abs(mapped - near)
        keep = d <= tol
        if keep.sum() < 3:
            break
        A = np.vstack([st[keep], np.ones(keep.sum())]).T
        sol, *_ = np.linalg.lstsq(A, near[keep], rcond=None)
        new_scale, new_offset = float(sol[0]), float(sol[1])
        if abs(new_scale - scale) < 1e-6 and abs(new_offset - offset) < 1e-6:
            scale, offset = new_scale, new_offset
            break
        scale, offset = new_scale, new_offset
    mapped = st * scale + offset
    idx = np.clip(np.searchsorted(onsets, mapped), 1, len(onsets) - 1)
    left, right = onsets[idx - 1], onsets[idx]
    near = np.where(np.abs(mapped - left) < np.abs(mapped - right), left, right)
    d = np.abs(mapped - near)
    keep = d <= tol
    rms_err = float(np.sqrt(np.mean(d[keep] ** 2))) if keep.any() else np.inf
    return offset, scale, int(keep.sum()), rms_err


def fit_from_first_anchor(onsets, strengths, score_times, anchor_score,
                          anchor_perf, scale_range=(0.85, 1.20), tol=0.080):
    """Fit the tempo with the first intro anchor pinned.

    The intro repeats every ~2 bars, so an unconstrained (offset, scale) search
    aliases onto a copy of the pattern a whole phrase away -- it matches more
    onsets while placing the first lyric bars too late. Pinning the performer's
    very first stroke to the score's first chord removes that whole family of
    solutions, and is exactly the "first intro anchor" the plan asks for.
    Offset then follows from the scale, so only tempo is searched.
    """
    st = np.asarray(score_times, dtype=float)
    best = None
    for sc in np.arange(scale_range[0], scale_range[1] + 1e-9, 0.0005):
        off = anchor_perf - anchor_score * sc
        mapped = st * sc + off
        idx = np.clip(np.searchsorted(onsets, mapped), 1, len(onsets) - 1)
        left, right = onsets[idx - 1], onsets[idx]
        d = np.minimum(np.abs(mapped - left), np.abs(mapped - right))
        hit = d <= tol
        n = int(hit.sum())
        if n < 4:
            continue
        rms_err = float(np.sqrt(np.mean(d[hit] ** 2)))
        val = n - 12.0 * rms_err
        if best is None or val > best[0]:
            best = (val, float(off), float(sc), n, rms_err)
    return best


def fit_affine(onsets, strengths, score_times, scale_range=(0.88, 1.14),
               offset_range=(-0.5, 4.0), tol=0.070):
    """Search (offset, scale) so score_t*scale + offset lands on real onsets.

    The scale range is deliberately narrow. A wide search happily locks onto a
    much faster tempo where the *denser* arpeggio subdivision lines up with the
    chord grid -- it matches more onsets while being musically wrong. Someone
    playing along to the score stays within roughly +/-15% of it.

    Matches are also required to span the take: a fit that only explains the
    first few seconds is rejected, which is the other way the dense-subdivision
    solution wins.
    """
    best = None
    scales = np.arange(scale_range[0], scale_range[1] + 1e-9, 0.001)
    offsets = np.arange(offset_range[0], offset_range[1] + 1e-9, 0.005)
    st = np.asarray(score_times)
    span_ref = float(st.max() - st.min()) if len(st) > 1 else 1.0
    for sc in scales:
        mapped_base = st * sc
        for off in offsets:
            mapped = mapped_base + off
            idx = np.searchsorted(onsets, mapped)
            idx = np.clip(idx, 1, len(onsets) - 1)
            left = onsets[idx - 1]
            right = onsets[idx]
            d = np.minimum(np.abs(mapped - left), np.abs(mapped - right))
            hit = d <= tol
            nhit = int(hit.sum())
            if nhit < 4:
                continue
            hit_scores = st[hit]
            span = float(hit_scores.max() - hit_scores.min()) / max(span_ref, 1e-9)
            if span < 0.5:
                continue
            near = np.where(np.abs(mapped - left) < np.abs(mapped - right),
                            idx - 1, idx)
            w = strengths[near]
            rms_err = float(np.sqrt(np.mean(d[hit] ** 2)))
            val = (float(np.sum(hit * (w + 0.5))) * (0.5 + span)
                   - 8.0 * rms_err * nhit)
            if best is None or val > best[0]:
                best = (val, float(off), float(sc), nhit, rms_err, span)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("guitar")
    ap.add_argument("out_dir")
    ap.add_argument("--compare", default=None,
                    help="offline-renderer run dir with phrase_events.csv")
    ap.add_argument("--guitar-gain", type=float, default=1.0)
    ap.add_argument("--vocal-gain", type=float, default=1.0)
    ap.add_argument("--anchor-perf", type=float, default=None,
                    help="manual time (s) of the first stroke in the take; "
                         "overrides the detector, which tends to latch onto "
                         "handling noise ahead of the first harmonic attack")
    ap.add_argument("--anchor-score-index", type=int, default=1,
                    help="chord_timeline index the manual anchor maps to")
    ap.add_argument("--scale-min", type=float, default=0.95)
    ap.add_argument("--scale-max", type=float, default=1.06)
    ap.add_argument("--offset", type=float, default=None,
                    help="use this score->performance offset instead of fitting")
    ap.add_argument("--scale", type=float, default=None,
                    help="use this score->performance scale instead of fitting")
    ap.add_argument("--piecewise-map", default=None,
                    help="piecewise_map.py output; overrides the affine fit "
                         "and tracks the player's drift")
    ap.add_argument("--pitch-semitones", type=float, default=0.0,
                    help="transpose the rendered vocal; the packaged vocal is "
                         "cut for the -7 key while the takes are played in the "
                         "original key, so +7 puts them back together")
    args = ap.parse_args()

    pkg = load_package(args.package)
    pkg_dir = os.path.dirname(os.path.abspath(args.package))
    micro = pkg["micro_phrases"]
    chords = pkg["chord_timeline"]
    os.makedirs(args.out_dir, exist_ok=True)

    print("[1/5] guitar + onsets")
    gtr, _ = read_audio(args.guitar)
    gtr = gtr.reshape(-1)
    onsets, strengths = detect_onsets(gtr)
    print("   detected %d onsets over %.2fs" % (len(onsets), len(gtr) / SR))

    segments = active_segments(gtr)
    print("   active playing segments: %s"
          % [(round(a, 2), round(b, 2)) for a, b in segments])
    main_a, main_b = max(segments, key=lambda s: s[1] - s[0])
    print("   fitting on the main segment %.2f-%.2fs" % (main_a, main_b))

    print("[2/5] fitting score->performance affine map")
    score_chord_t = np.array([c["start_sec"] for c in chords])
    # only chord events that can plausibly land inside the main segment
    usable = score_chord_t[score_chord_t <= (main_b - main_a) * 1.25]
    mask = (onsets >= main_a - 0.2) & (onsets <= main_b + 0.2)
    in_main = onsets[mask]

    # Anchor 1: the performer's first stroke == the score's first real chord.
    # chord_timeline[0] is the "N" (no chord) marker at 0.000s.
    anchor_score = float(chords[args.anchor_score_index]["start_sec"])
    anchor_perf = (args.anchor_perf if args.anchor_perf is not None
                   else float(in_main[0]))
    print("   anchor 1: score %.4fs <- first stroke %.4fs%s"
          % (anchor_score, anchor_perf,
             " (manual)" if args.anchor_perf is not None else " (detected)"))

    pw_doc = None
    if args.piecewise_map:
        from piecewise_map import map_score_to_performance
        with open(args.piecewise_map, encoding="utf-8") as f:
            pw_doc = json.load(f)
        offset, scale = float("nan"), float("nan")
        hits, rms_err = -1, float("nan")
        print("   using piecewise map with %d anchors (%s)"
              % (len(pw_doc["anchors"]), args.piecewise_map))
    elif args.offset is not None and args.scale is not None:
        offset, scale = args.offset, args.scale
        hits, rms_err = -1, float("nan")
        print("   using supplied map: offset=%.4fs scale=%.4f (tempo x%.4f)"
              % (offset, scale, 1.0 / scale))
    else:
        best = fit_from_first_anchor(in_main, strengths[mask], usable,
                                     anchor_score, anchor_perf,
                                     scale_range=(args.scale_min, args.scale_max),
                                     tol=0.045)
        if best is None:
            raise SystemExit("no affine fit found")
        val, offset, scale, hits, rms_err = best
        print("   coarse : offset=%.4f scale=%.4f  matched %d  rms %.1f ms"
              % (offset, scale, hits, rms_err * 1000))
        offset, scale, hits, rms_err = icp_refine(in_main, usable, offset, scale)
        if abs((anchor_score * scale + offset) - anchor_perf) > 0.15:
            print("   ICP drifted off anchor 1; keeping the anchored fit")
            val, offset, scale, hits, rms_err = best
        print("   refined: offset=%.4fs scale=%.4f  (performance tempo x%.4f)"
              % (offset, scale, 1.0 / scale))
        print("   matched %d/%d chord events, rms error %.1f ms"
              % (hits, len(usable), rms_err * 1000))

    if pw_doc is not None:
        from piecewise_map import map_score_to_performance

        def score_to_perf(t):
            return map_score_to_performance(pw_doc, t)
    else:
        def score_to_perf(t):
            return t * scale + offset

    # ---- anchors: nearest real onset to two structural score boundaries ----
    def snap(score_t):
        p = score_to_perf(score_t)
        if len(onsets) == 0:
            return p, None, 0.0
        i = int(np.argmin(np.abs(onsets - p)))
        return p, float(onsets[i]), float(onsets[i] - p)

    anchor_scores = [chords[1]["start_sec"], chords[9]["start_sec"]]
    anchors = []
    for s_t in anchor_scores:
        pred, snapped, err = snap(s_t)
        anchors.append({
            "score_sec": round(float(s_t), 4),
            "predicted_performance_sec": round(pred, 4),
            "nearest_detected_onset_sec": round(snapped, 4) if snapped else None,
            "snap_error_ms": round(err * 1000.0, 1),
        })

    anchors_doc = {
        "guitar_file": os.path.abspath(args.guitar),
        "package": os.path.abspath(args.package),
        "method": "exhaustive (offset, tempo) search over the score chord grid",
        "verification": ("visually verified against waveform/spectrogram; "
                         "NOT verified by listening -- the agent cannot play audio"),
        "affine": {"offset_sec": round(offset, 6),
                   "scale_score_to_performance": round(scale, 6),
                   "performance_tempo_ratio": round(1.0 / scale, 6),
                   "matched_chord_events": hits,
                   "candidate_chord_events": int(len(usable)),
                   "match_rms_error_ms": round(rms_err * 1000.0, 2)},
        "anchors": anchors,
        "detected_onset_count": int(len(onsets)),
        "active_segments_sec": [[round(a, 3), round(b, 3)] for a, b in segments],
        "fitted_on_segment_sec": [round(main_a, 3), round(main_b, 3)],
        "note": ("A single affine map only describes the segment it was fitted "
                 "on. Phrases outside the active segments are rendered but must "
                 "not be used for timing statistics -- the player had stopped."),
    }
    with open(os.path.join(args.out_dir, "manual_anchors.json"), "w",
              encoding="utf-8") as f:
        json.dump(anchors_doc, f, ensure_ascii=False, indent=2)

    print("[3/5] scheduling %d phrases" % len(micro))
    total = len(gtr)
    placements, clips, rows = [], [], []
    for i, m in enumerate(micro):
        v = m["vocal"]
        s_start = float(m["score"]["start_sec"])
        p_start = score_to_perf(s_start)
        target = int(round((p_start - float(v["content_offset_sec"])) * SR))
        if target >= total or target + 1 < 0:
            continue
        c, _ = read_audio(os.path.join(pkg_dir, v["directory"], v["file"]))
        clips.append(c.reshape(-1))
        placements.append(Placement(
            phrase_id=m["phrase_id"], index=i, clip_path=v["file"],
            target_sample=target, score_start_sec=s_start,
            score_end_sec=float(m["score"]["end_sec"])))
        rows.append({
            "index": i,
            "phrase_id": m["phrase_id"],
            "lyrics": m["lyrics"],
            "score_start_sec": round(s_start, 4),
            "oracle_performance_sec": round(p_start, 4),
            "target_sample": target,
        })
    print("   %d phrases fall inside the take" % len(placements))

    vocal = overlap_add(placements, clips, total, crossfade=True)
    if abs(args.pitch_semitones) > 1e-6:
        import librosa
        print("   transposing vocal by %+.1f semitones" % args.pitch_semitones)
        vocal = librosa.effects.pitch_shift(
            vocal.astype(np.float32), sr=SR, n_steps=args.pitch_semitones
        ).astype(np.float64)
        vocal = vocal[:total] if len(vocal) >= total else np.pad(
            vocal, (0, total - len(vocal)))
    write_audio(os.path.join(args.out_dir, "oracle_vocal.wav"), vocal)
    mix = args.guitar_gain * gtr + args.vocal_gain * vocal
    write_audio(os.path.join(args.out_dir, "guitar_plus_oracle_vocal.wav"), mix)

    print("[4/5] comparing against the shipped follower")
    if args.compare:
        ev = os.path.join(args.compare, "phrase_events.csv")
        actual = {}
        with open(ev, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                actual.setdefault(int(r["phrase_index"]), float(r["time_sec"]))
        beat = 60.0 / float(pkg["score_bpm"])
        deltas = []
        for r in rows:
            a = actual.get(r["index"])
            inside = any(lo <= r["oracle_performance_sec"] <= hi
                         for lo, hi in segments)
            r["inside_active_segment"] = int(inside)
            if a is None:
                r["follower_sec"] = ""
                r["follower_minus_oracle_ms"] = ""
                r["follower_minus_oracle_beats"] = ""
                continue
            d = a - r["oracle_performance_sec"]
            r["follower_sec"] = round(a, 4)
            r["follower_minus_oracle_ms"] = round(d * 1000.0, 1)
            r["follower_minus_oracle_beats"] = round(d / beat, 3)
            if inside:
                deltas.append(d)
        if deltas:
            ds = sorted(deltas)
            n = len(ds)
            print("   phrases compared (inside active playing only): %d" % n)
            print("   follower - oracle:  median %+.0f ms (%.2f beat)"
                  % (ds[n // 2] * 1000, ds[n // 2] / beat))
            print("                       p95    %+.0f ms (%.2f beat)"
                  % (ds[int(0.95 * (n - 1))] * 1000,
                     ds[int(0.95 * (n - 1))] / beat))
            print("                       max    %+.0f ms (%.2f beat)"
                  % (ds[-1] * 1000, ds[-1] / beat))
            print("   late by >= half a beat: %d/%d"
                  % (sum(1 for d in ds if d >= beat / 2), n))
            print("   late by >= one beat   : %d/%d"
                  % (sum(1 for d in ds if d >= beat), n))
    else:
        for r in rows:
            r["follower_sec"] = ""
            r["follower_minus_oracle_ms"] = ""
            r["follower_minus_oracle_beats"] = ""

    print("[5/5] writing csv")
    path = os.path.join(args.out_dir, "phrase_targets.csv")
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("wrote", args.out_dir)


if __name__ == "__main__":
    main()
