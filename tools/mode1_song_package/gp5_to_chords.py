#!/usr/bin/env python3
"""Infer a compact, source-aligned chord timeline from a guitar GP5 file."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import guitarpro
import numpy as np

from attach_gp5_tab_hints import align_groups_to_audio, extract_note_groups


ROOT_NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
QUALITIES = {
    "": {0, 4, 7},
    "m": {0, 3, 7},
    "7": {0, 4, 7, 10},
    "maj7": {0, 4, 7, 11},
    "m7": {0, 3, 7, 10},
    "dim": {0, 3, 6},
    "sus4": {0, 5, 7},
}
ALLOWED_CHORDS = {
    (0, ""),
    (5, "maj7"),
    (5, ""),
    (7, ""),
    (9, "m"),
    (4, ""),
    (4, "7"),
    (5, "m"),
    (5, "m7"),
    (8, "dim"),
    (7, "7"),
}


def classify(pitch_counts: np.ndarray, bass_counts: np.ndarray) -> str:
    present = {index for index, value in enumerate(pitch_counts) if value > 0}
    if not present:
        return "N"
    best = None
    for root in range(12):
        for quality, intervals in QUALITIES.items():
            if (root, quality) not in ALLOWED_CHORDS:
                continue
            chord = {(root + interval) % 12 for interval in intervals}
            covered = sum(pitch_counts[list(chord)])
            outside = sum(pitch_counts[list(present - chord)]) if present - chord else 0
            missing = len(chord - present)
            bass_bonus = bass_counts[root] * 0.6
            score = covered * 1.4 - outside * 0.75 - missing * 0.8 + bass_bonus
            candidate = (float(score), -len(chord), root, quality)
            if best is None or candidate > best:
                best = candidate
    assert best is not None
    return ROOT_NAMES[best[2]] + best[3]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gp5", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--window-quarters", type=float, default=2.0)
    args = parser.parse_args()

    song = guitarpro.parse(str(args.gp5))
    groups = extract_note_groups(song)
    alignment = align_groups_to_audio(groups, args.audio)
    seconds_per_quarter = 60.0 / float(song.tempo)
    window_seconds = args.window_quarters * seconds_per_quarter
    final_raw = groups[-1]["raw_sec"] + window_seconds
    raw_starts = np.arange(0.0, final_raw, window_seconds)
    group_raw = np.asarray([group["raw_sec"] for group in groups])
    group_mapped = np.asarray([group["start_sec"] for group in groups])

    events = [{"start_sec": 0.0, "chord": "N"}]
    for raw_start in raw_starts:
        raw_end = raw_start + window_seconds
        selected = [
            group
            for group in groups
            if raw_start <= group["raw_sec"] < raw_end
        ]
        if not selected:
            continue
        pitch_counts = np.zeros(12, dtype=np.float32)
        bass_counts = np.zeros(12, dtype=np.float32)
        for group in selected:
            for note in group["notes"]:
                pitch_counts[note["pitch_class"]] += 1.0
            bass_counts[min(note["midi"] for note in group["notes"]) % 12] += 1.0
        chord = classify(pitch_counts, bass_counts)
        mapped_start = float(
            np.interp(raw_start, group_raw, group_mapped)
        )
        events.append({"start_sec": round(mapped_start, 6), "chord": chord})

    result = {
        "schema_version": 1,
        "song": args.output.parent.name,
        "bpm": float(song.tempo),
        "source": args.gp5.name,
        "alignment": alignment,
        "chords": events,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"{args.output}: {len(events)} chord events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
