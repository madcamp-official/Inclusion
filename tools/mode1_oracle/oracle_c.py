"""Oracle C -- does micro slicing / crossfade / content offset damage the vocal?

Four renders on the *same* correct clock, so any difference between them is
caused by the reassembly path and not by timing:

  1. separated_vocal.wav            original separated vocal
  2. converted_full.wav             RVC converted vocal, uncut
  3. reconstructed_no_crossfade.wav micro clips, hard cuts, no gain ramps
  4. reconstructed_current_player.wav  micro clips through an emulation of the
                                    current PhrasePlayer gain path

PhrasePlayer emulation (src/mode1_vocal_follower/PhrasePlayer.cpp):
  - playback starts at contentOffsetSamples, i.e. the 80 ms lead-in pad is
    skipped and the clip begins at score.start
  - fade-in is equal-power sin over 35 ms when a previous voice is still
    sounding, otherwise 20 ms
  - the outgoing voice fades out equal-power over 35 ms and is then reset
  - a tail edge-fade ramps the last 35 ms of the playable window
  - accentGain = 0.92 + 0.16 * accent, and accent defaults to 0.5 -> 1.0

SoundTouch is not emulated. At score-native timing requestedDuration equals the
natural duration, so elasticTempo is 1.0 and no stretching happens; pitch shift
is likewise identity here. Both are exercised only by the live follower.

Usage:
  python oracle_c.py <song_package.json> <output-dir>
"""
from __future__ import annotations

import csv
import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, db, load_package, read_audio, write_audio

FADE_IN_FRESH = 0.020
FADE_TRANSITION = 0.035
END_FADE = 0.035
ACCENT_GAIN = 0.92 + 0.16 * 0.5


def equal_power(lin: np.ndarray) -> np.ndarray:
    return np.sin(np.pi / 2.0 * np.clip(lin, 0.0, 1.0))


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x * x))) if x.size else 0.0


def render(micro, clips, total, current_player: bool):
    """Place every phrase at its score time; optionally apply the player gains."""
    out = np.zeros(total, dtype=np.float64)
    off = int(round(0.08 * SR))
    fade_tr = int(round(FADE_TRANSITION * SR))
    fade_fresh = int(round(FADE_IN_FRESH * SR))
    end_fade = int(round(END_FADE * SR))
    notes = []

    starts = [int(round(float(m["score"]["start_sec"]) * SR)) for m in micro]

    for i, (m, clip) in enumerate(zip(micro, clips)):
        content_off = int(round(float(m["vocal"]["content_offset_sec"]) * SR))
        body = clip[content_off:]                    # from score.start onward
        start = starts[i]
        playback_len = len(body)

        # the next phrase kills this voice after a transition fade
        kill_at = playback_len
        prev_alive = False
        if i + 1 < len(starts):
            until_next = starts[i + 1] - start
            if until_next < playback_len:
                kill_at = min(playback_len, until_next + fade_tr)
        if i > 0:
            prev_end = starts[i - 1] + len(clips[i - 1]) - content_off
            prev_alive = prev_end > start

        seg = body[:kill_at].copy()
        n = len(seg)
        if n == 0:
            continue

        if current_player:
            t = np.arange(n, dtype=np.float64)
            fin_len = fade_tr if prev_alive else fade_fresh
            fade_in = equal_power(t / max(1, fin_len))
            remaining = playback_len - t
            edge = equal_power(remaining / max(1, end_fade))
            gain = fade_in * edge * ACCENT_GAIN
            if i + 1 < len(starts) and kill_at < playback_len:
                fo = np.ones(n)
                k = min(fade_tr, n)
                fo[n - k:] = equal_power(np.arange(k, 0, -1) / max(1, fade_tr))
                gain = gain * fo
            shaped = seg * gain
        else:
            gain = np.ones(n)
            shaped = seg

        a, b = max(0, start), min(total, start + n)
        if b > a:
            out[a:b] += shaped[a - start:b - start]

        # ---- per-phrase measurements ----
        pad = clip[:content_off]
        head = body[:int(0.20 * SR)]
        first35 = seg[:min(fade_tr, n)]
        first35_shaped = shaped[:min(fade_tr, n)]
        peak = float(np.max(np.abs(clip))) if clip.size else 0.0
        kill_level = float(np.abs(seg[-1])) if n else 0.0
        notes.append({
            "index": i,
            "phrase_id": m["phrase_id"],
            "lyrics": m["lyrics"],
            "score_start_sec": round(float(m["score"]["start_sec"]), 4),
            "score_end_sec": round(float(m["score"]["end_sec"]), 4),
            "clip_duration_sec": round(len(clip) / SR, 4),
            "playable_after_offset_sec": round(playback_len / SR, 4),
            "played_sec": round(n / SR, 4),
            "truncated_by_next_ms": round((playback_len - kill_at) / SR * 1000, 1),
            "prev_voice_alive": int(prev_alive),
            "fade_in_ms": round((fade_tr if prev_alive else fade_fresh) / SR * 1000, 1),
            # energy thrown away by skipping the lead-in pad
            "discarded_pad_rms_db": round(db(rms(pad) / max(rms(head), 1e-12)), 2),
            # attack softened by the fade-in
            "fadein_loss_db": round(db(rms(first35_shaped) / max(rms(first35), 1e-12)), 2),
            "level_at_start_pct_of_peak": round(
                100.0 * float(np.abs(body[0])) / max(peak, 1e-12), 1) if playback_len else 0.0,
            "kill_level_pct_of_peak": round(100.0 * kill_level / max(peak, 1e-12), 1),
            "overlap_with_next_ms": round(
                max(0, (start + playback_len) - starts[i + 1]) / SR * 1000, 1)
            if i + 1 < len(starts) else 0.0,
            "gap_to_next_ms": round(
                max(0, starts[i + 1] - (start + playback_len)) / SR * 1000, 1)
            if i + 1 < len(starts) else 0.0,
        })
    return out, notes


def main(package_path: str, out_dir: str):
    pkg = load_package(package_path)
    pkg_dir = os.path.dirname(os.path.abspath(package_path))
    micro = pkg["micro_phrases"]
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(os.path.join(out_dir, "suspicious_clips"), exist_ok=True)

    print("[1/4] references")
    sep, _ = read_audio(pkg["audio"]["source_vocal"])
    conv, _ = read_audio(pkg["audio"]["converted_vocal"])
    write_audio(os.path.join(out_dir, "separated_vocal.wav"), sep)
    write_audio(os.path.join(out_dir, "converted_full.wav"), conv)
    total = max(len(sep), len(conv))

    print("[2/4] clips")
    clips = []
    for m in micro:
        v = m["vocal"]
        c, _ = read_audio(os.path.join(pkg_dir, v["directory"], v["file"]))
        clips.append(c.reshape(-1))

    print("[3/4] renders")
    nofade, notes_nf = render(micro, clips, total, current_player=False)
    player, notes_pl = render(micro, clips, total, current_player=True)
    write_audio(os.path.join(out_dir, "reconstructed_no_crossfade.wav"), nofade)
    write_audio(os.path.join(out_dir, "reconstructed_current_player.wav"), player)

    print("[4/4] measurements")
    conv_flat = conv.reshape(-1)
    for r, _n in zip(notes_pl, notes_nf):
        a = int(r["score_start_sec"] * SR)
        b = min(int(r["score_end_sec"] * SR), len(conv_flat), len(player))
        if b <= a:
            r["player_vs_converted_db"] = ""
            r["nofade_vs_converted_db"] = ""
            continue
        ref = conv_flat[a:b]
        rr = rms(ref)
        r["player_vs_converted_db"] = round(
            db(rms(ref - player[a:b]) / max(rr, 1e-12)), 2)
        r["nofade_vs_converted_db"] = round(
            db(rms(ref - nofade[a:b]) / max(rr, 1e-12)), 2)

    # flag the clips worth looking at
    for r in notes_pl:
        why = []
        if r["discarded_pad_rms_db"] > -6.0:
            why.append("loud_discarded_lead_in")
        if r["kill_level_pct_of_peak"] > 30.0:
            why.append("tail_cut_mid_syllable")
        if r["truncated_by_next_ms"] > 60.0:
            why.append("truncated_by_next_phrase")
        if r["player_vs_converted_db"] != "" and r["player_vs_converted_db"] > -12.0:
            why.append("poor_reconstruction")
        r["suspicious"] = ";".join(why)

    path = os.path.join(out_dir, "clip_timing.csv")
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(notes_pl[0].keys()))
        w.writeheader()
        w.writerows(notes_pl)

    sus = [r for r in notes_pl if r["suspicious"]]
    for r in sus:
        src = os.path.join(pkg_dir, micro[r["index"]]["vocal"]["directory"],
                           micro[r["index"]]["vocal"]["file"])
        shutil.copy2(src, os.path.join(out_dir, "suspicious_clips",
                                       micro[r["index"]]["vocal"]["file"]))

    def stat(key):
        v = [r[key] for r in notes_pl if r[key] != ""]
        v.sort()
        return (v[0], v[len(v) // 2], v[int(0.95 * (len(v) - 1))], v[-1])

    print()
    print("phrases: %d   suspicious: %d" % (len(notes_pl), len(sus)))
    for k in ("discarded_pad_rms_db", "fadein_loss_db",
              "level_at_start_pct_of_peak", "kill_level_pct_of_peak",
              "truncated_by_next_ms", "player_vs_converted_db",
              "nofade_vs_converted_db"):
        lo, med, p95, hi = stat(k)
        print("  %-28s min %8.2f  median %8.2f  p95 %8.2f  max %8.2f"
              % (k, lo, med, p95, hi))
    from collections import Counter
    c = Counter(w for r in sus for w in r["suspicious"].split(";"))
    print("  reasons:", dict(c))
    print("wrote", path)
    return notes_pl


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
