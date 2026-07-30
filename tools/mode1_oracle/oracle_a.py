"""Oracle A -- replay the packaged micro vocals on the *correct* clock.

No guitar input, no onset detector, no chord follower. Every micro phrase is
placed at the immutable score time recorded in song_package.json, so the only
things under test are the vocal assets themselves and the score<->source
mapping.

Because all micro clips are cut from one converted vocal, a linear crossfade
over the padded overlaps must rebuild that vocal sample-for-sample. Any
residual is a slicing or timing defect, not a follower defect.

Usage:
  python oracle_a.py <song_package.json> <output-dir>
"""
from __future__ import annotations

import csv
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import (SR, audible_onset, db, load_package, overlap_add,
                           read_audio, write_audio, Placement)

ORIGINAL_MP3 = (r"C:\Users\User\Downloads\tuki. - 만찬가(晩餐歌) "
                r"[가사발음해석] - 지구의 가사집 [J-POP] (128k).mp3")


def build(package_path: str, out_dir: str):
    pkg = load_package(package_path)
    pkg_dir = os.path.dirname(os.path.abspath(package_path))
    micro = pkg["micro_phrases"]

    os.makedirs(out_dir, exist_ok=True)

    # ---------------- references ----------------
    print("[1/6] references")
    orig, _ = read_audio(ORIGINAL_MP3, mono=False)
    write_audio(os.path.join(out_dir, "original_reference.wav"), orig)

    sep, _ = read_audio(pkg["audio"]["source_vocal"])
    write_audio(os.path.join(out_dir, "separated_vocal_reference.wav"), sep)

    conv, _ = read_audio(pkg["audio"]["converted_vocal"])
    write_audio(os.path.join(out_dir, "converted_full_reference.wav"), conv)

    total = max(len(orig), len(sep), len(conv))
    conv_flat = conv.reshape(-1)

    # ---------------- load clips + measure ----------------
    print("[2/6] loading %d micro clips" % len(micro))
    clips, rows = [], []
    for i, m in enumerate(micro):
        v = m["vocal"]
        clip_path = os.path.join(pkg_dir, v["directory"], v["file"])
        c, _ = read_audio(clip_path)
        c = c.reshape(-1)
        clips.append(c)

        meas = audible_onset(c)
        pkg_anchor = float(v.get("sync", {}).get("audible_onset_sec", 0.0))
        content_off = float(v["content_offset_sec"])
        score_start = float(m["score"]["start_sec"])
        score_end = float(m["score"]["end_sec"])

        expect_len = (score_end - score_start) + 2.0 * content_off
        rows.append({
            "index": i,
            "phrase_id": m["phrase_id"],
            "lyrics": m["lyrics"],
            "score_start_sec": round(score_start, 6),
            "score_end_sec": round(score_end, 6),
            "clip_start_sec": round(float(v["clip_start_sec"]), 6),
            "clip_end_sec": round(float(v["clip_end_sec"]), 6),
            "content_offset_sec": content_off,
            "clip_duration_sec": round(len(c) / SR, 6),
            "expected_duration_sec": round(expect_len, 6),
            "duration_error_ms": round((len(c) / SR - expect_len) * 1000.0, 3),
            "package_audible_onset_sec": pkg_anchor,
            "measured_audible_onset_sec": (round(meas, 6)
                                           if meas is not None else ""),
            "measured_minus_content_offset_ms": (
                round((meas - content_off) * 1000.0, 3)
                if meas is not None else ""),
            "package_minus_measured_ms": (
                round((pkg_anchor - meas) * 1000.0, 3)
                if meas is not None else ""),
            "clip_peak": round(float(np.max(np.abs(c))) if c.size else 0.0, 6),
        })

    # ---------------- placements ----------------
    def place(offset_fn) -> list[Placement]:
        out = []
        for i, m in enumerate(micro):
            v = m["vocal"]
            score_start = float(m["score"]["start_sec"])
            lead = offset_fn(i, m, v)
            out.append(Placement(
                phrase_id=m["phrase_id"], index=i,
                clip_path=v["file"],
                target_sample=int(round((score_start - lead) * SR)),
                score_start_sec=score_start,
                score_end_sec=float(m["score"]["end_sec"])))
        return out

    print("[3/6] content_offset policy")
    p_content = place(lambda i, m, v: float(v["content_offset_sec"]))
    recon = overlap_add(p_content, clips, total, crossfade=True)
    write_audio(os.path.join(out_dir, "micro_reconstructed.wav"), recon)

    print("[4/6] package audible_onset policy")
    p_pkg = place(lambda i, m, v: float(
        v.get("sync", {}).get("audible_onset_sec", 0.0)))
    recon_pkg = overlap_add(p_pkg, clips, total, crossfade=True)
    write_audio(os.path.join(out_dir,
                             "micro_reconstructed_with_audible_anchor.wav"),
                recon_pkg)

    print("[5/6] measured audible_onset policy (extra)")
    meas_map = {r["index"]: r["measured_audible_onset_sec"] for r in rows}
    p_meas = place(lambda i, m, v: (meas_map[i] if meas_map[i] != ""
                                    else float(v["content_offset_sec"])))
    recon_meas = overlap_add(p_meas, clips, total, crossfade=True)
    write_audio(os.path.join(out_dir,
                             "micro_reconstructed_measured_anchor.wav"),
                recon_meas)

    # no-crossfade variant makes the padded overlaps audible as doubling
    recon_sum = overlap_add(p_content, clips, total, crossfade=False)
    write_audio(os.path.join(out_dir, "micro_reconstructed_summed_nofade.wav"),
                recon_sum)

    # ---------------- fidelity vs converted full ----------------
    print("[6/6] fidelity + csv + report")
    v0 = int(micro[0]["score"]["start_sec"] * SR)
    v1 = int(micro[-1]["score"]["end_sec"] * SR)
    n = min(len(conv_flat), len(recon), v1)

    def fidelity(sig):
        a = conv_flat[v0:n]
        b = sig[v0:n]
        d = a - b
        return {
            "rms_ref": float(np.sqrt(np.mean(a * a))),
            "rms_diff": float(np.sqrt(np.mean(d * d))),
            "max_abs_diff": float(np.max(np.abs(d))),
        }

    fid = {
        "content_offset": fidelity(recon),
        "package_audible_anchor": fidelity(recon_pkg),
        "measured_audible_anchor": fidelity(recon_meas),
        "summed_no_crossfade": fidelity(recon_sum),
    }
    for k, f in fid.items():
        f["diff_to_ref_db"] = round(db(f["rms_diff"] / max(f["rms_ref"], 1e-12)), 2)

    # per-phrase reconstruction error in the score window
    for r in rows:
        a = int(r["score_start_sec"] * SR)
        b = min(int(r["score_end_sec"] * SR), n)
        if b <= a:
            r["window_diff_db"] = ""
            continue
        ref = conv_flat[a:b]
        d = ref - recon[a:b]
        rr = float(np.sqrt(np.mean(ref * ref)))
        dd = float(np.sqrt(np.mean(d * d)))
        r["window_diff_db"] = round(db(dd / max(rr, 1e-12)), 2)

    csv_path = os.path.join(out_dir, "phrase_anchor_errors.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    summary = {
        "package": package_path,
        "micro_phrase_count": len(micro),
        "reviewed_vowel_onset_count": sum(
            1 for m in micro if "vowel_onset_sec" in m["vocal"].get("sync", {})),
        "content_offset_values": sorted({float(m["vocal"]["content_offset_sec"])
                                         for m in micro}),
        "fidelity_vs_converted_full": fid,
        "duration_error_ms": {
            "max_abs": max(abs(r["duration_error_ms"]) for r in rows),
        },
    }
    with open(os.path.join(out_dir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    print(json.dumps(summary["fidelity_vs_converted_full"], indent=2))
    print("duration_error_ms max_abs:", summary["duration_error_ms"]["max_abs"])
    print("wrote", out_dir)
    return summary


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
