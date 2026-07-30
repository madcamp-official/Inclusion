"""Rebuild a Mode 1 song package with a wider vocal lead-in, and optionally in
the guitar's original key.

Why this exists
----------------
Two independent defects surfaced during the Mode 1 lag investigation
(docs/MODE1_ORIGINAL_VS_CURRENT_DIAGNOSIS_AND_REBUILD_PLAN_2026-07-29.md,
output/mode1_oracle/oracle_a and oracle_c):

1. Micro clips carry only an 80 ms pad before the scored vowel
   (`--micro-margin-sec` in build_song_package.py). The sung consonant
   commonly starts ~150-200 ms before the vowel, so it is cut out of the clip
   entirely -- the vowel is technically on time but arrives bare, which reads
   as "late". Widening the pad to `--lead-in-sec` (default 0.25 s, confirmed
   by ear against the previous_take guitar recording) fixes this for ANY
   song and does not depend on key.

2. `bansanka` specifically was rendered with `base_key_shift = -7`.
   `build_song_package.py` transposes `chord_timeline[].chord` by that amount,
   but both real guitar takes were played in the guitar's ORIGINAL key.
   `chord` (not `raw_chord`) is what `SongPackage.cpp`/`Mode1Controller.cpp`
   match against, so chord confirmation can never succeed and the follower
   degrades to onset-only tracking. This only matters when base_key_shift is
   not a multiple of 12 (semitone shifts by a full octave leave chord names
   unchanged) -- Oasis's `-12` is octave-only and does not have this problem,
   so `--semitones` defaults to 0 and this step is skipped unless requested.

This tool never modifies the input package or its audio; it always writes to
a separate output directory.

Usage:
  python rebuild_package_in_original_key.py <in_package.json> <out_dir>
                                            [--semitones 7] [--lead-in-sec 0.25]
"""
from __future__ import annotations

import argparse
import copy
import json
import os

import numpy as np
import soundfile as sf

SR = 48000


def load_full_vocal(path: str, semitones: float) -> np.ndarray:
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    x = x.mean(axis=1).astype(np.float64)
    if sr != SR:
        import librosa
        x = librosa.resample(x.astype(np.float32), orig_sr=sr, target_sr=SR).astype(np.float64)
    if abs(semitones) > 1e-6:
        import librosa
        x = librosa.effects.pitch_shift(
            x.astype(np.float32), sr=SR, n_steps=semitones
        ).astype(np.float64)
    return x


def recut(full: np.ndarray, score_start: float, score_end: float,
         lead_in: float, trail: float, out_path: str) -> dict:
    """Slice fresh from score time, independent of whatever the old clip used."""
    clip_start = max(0.0, score_start - lead_in)
    clip_end = min(len(full) / SR, score_end + trail)
    a = int(round(clip_start * SR))
    b = int(round(clip_end * SR))
    seg = full[a:b].copy()
    want = b - a
    if len(seg) < want:
        seg = np.pad(seg, (0, want - len(seg)))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    sf.write(out_path, seg, SR, subtype="PCM_24")
    return {
        "clip_start_sec": round(clip_start, 6),
        "clip_end_sec": round(clip_end, 6),
        "content_offset_sec": round(score_start - clip_start, 6),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("in_package")
    ap.add_argument("out_dir")
    ap.add_argument("--semitones", type=float, default=0.0,
                    help="pitch-shift the converted vocal by this many "
                         "semitones and switch chord_timeline[].chord to "
                         "raw_chord. Only meaningful when base_key_shift is "
                         "not a multiple of 12 (an octave shift leaves chord "
                         "names unchanged, e.g. Oasis's -12).")
    ap.add_argument("--lead-in-sec", type=float, default=0.25,
                    help="seconds of vocal audio to keep before the scored "
                         "vowel in every re-cut clip")
    ap.add_argument("--trail-sec", type=float, default=0.25,
                    help="seconds of vocal audio to keep after the scored "
                         "phrase end in every re-cut clip")
    args = ap.parse_args()

    in_dir = os.path.dirname(os.path.abspath(args.in_package))
    out_dir = os.path.abspath(args.out_dir)
    if os.path.abspath(in_dir) == out_dir:
        raise SystemExit("refusing to overwrite the source package")
    os.makedirs(out_dir, exist_ok=True)

    with open(args.in_package, encoding="utf-8") as f:
        pkg = json.load(f)
    out = copy.deepcopy(pkg)

    print("[1/4] loading converted vocal%s"
          % (" and transposing %+g semitones" % args.semitones
             if abs(args.semitones) > 1e-6 else ""))
    full = load_full_vocal(pkg["audio"]["converted_vocal"], args.semitones)
    if abs(args.semitones) > 1e-6:
        dst_vocal = os.path.join(out_dir, "converted_vocal_transposed.wav")
        sf.write(dst_vocal, full, SR, subtype="PCM_24")
        out["audio"]["converted_vocal"] = dst_vocal

    print("[2/4] re-cutting %d micro clips (lead-in %.0f ms, trail %.0f ms)"
          % (len(pkg["micro_phrases"]), args.lead_in_sec * 1000, args.trail_sec * 1000))
    micro_rel = os.path.join("fine_candidate", "micro_vocals")
    micro_out = os.path.join(out_dir, micro_rel)
    for m in out["micro_phrases"]:
        v = m["vocal"]
        meta = recut(full, float(m["score"]["start_sec"]), float(m["score"]["end_sec"]),
                    args.lead_in_sec, args.trail_sec,
                    os.path.join(micro_out, v["file"]))
        v.update(meta)
        v["directory"] = "fine_candidate/micro_vocals"

    n_phrase_clips = 0
    for p in out.get("phrases", []):
        v = p.get("vocal") or {}
        if not v.get("file"):
            continue
        d = os.path.join(out_dir, "fine_candidate", "phrase_vocals")
        meta = recut(full, float(p["score"]["start_sec"]), float(p["score"]["end_sec"]),
                    args.lead_in_sec, args.trail_sec,
                    os.path.join(d, v["file"]))
        v.update(meta)
        v["directory"] = "fine_candidate/phrase_vocals"
        n_phrase_clips += 1
    if n_phrase_clips:
        print("     also re-cut %d phrase-level clips" % n_phrase_clips)

    fix_key = abs(args.semitones) > 1e-6
    if fix_key:
        print("[3/4] rewriting chord labels to the original key")
        swapped = 0

        def use_raw(container):
            nonlocal swapped
            for c in container:
                raw = c.get("raw_chord")
                if raw and raw != "N.C.":
                    if c.get("chord") != raw:
                        swapped += 1
                    c["chord"] = raw
                elif raw == "N.C.":
                    c["chord"] = "N"

        use_raw(out.get("chord_timeline", []))
        for p in out.get("phrases", []):
            use_raw(p.get("chords", []))
        for m in out.get("micro_phrases", []):
            use_raw(m.get("chords", []))
        print("     %d chord labels changed" % swapped)

        out["base_key_shift"] = 0
        if isinstance(out.get("vocal_style"), dict):
            out["vocal_style"]["base_key_shift"] = 0
    else:
        print("[3/4] skipping chord-key fix (--semitones is 0)")

    out["rebuilt_from"] = os.path.abspath(args.in_package)
    out["rebuild_note"] = (
        "lead-in widened to %.0f ms / trail %.0f ms (was 80 ms)%s"
        % (args.lead_in_sec * 1000, args.trail_sec * 1000,
           (("; chord labels switched to raw_chord and vocal transposed "
             "%+g semitones so the packaged key matches guitar takes played "
             "in the original key" % args.semitones) if fix_key else "")))

    dst_pkg = os.path.join(out_dir, "song_package.json")
    with open(dst_pkg, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print("[4/4] wrote", dst_pkg)

    for k in ("phrases", "micro_phrases", "chord_timeline"):
        print("     %-15s %d" % (k, len(out.get(k, []))))
    print("     base_key_shift  %s -> %s"
          % (pkg["base_key_shift"], out["base_key_shift"]))
    print("     first chord     %.3f" % out["chord_timeline"][0]["start_sec"])
    print("     first vocal     %.3f" % out["micro_phrases"][0]["score"]["start_sec"])


if __name__ == "__main__":
    main()
