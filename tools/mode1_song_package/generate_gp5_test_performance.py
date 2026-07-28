#!/usr/bin/env python3
"""Render the attached GP5 TAB hints as a deterministic picked-guitar WAV."""

from __future__ import annotations

import argparse
import json
import math
import wave
from pathlib import Path

import numpy as np

from generate_guitar_test_performance import (
    add_plucked_string,
    read_reference_stats,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("song_package_with_tab", type=Path)
    parser.add_argument("output_wav", type=Path)
    parser.add_argument("--reference-wav", type=Path)
    parser.add_argument("--seed", type=int, default=510)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    args = parser.parse_args()

    package = json.loads(
        args.song_package_with_tab.read_text(encoding="utf-8")
    )
    hints = package["tab_tracking"]["chord_hints"]
    final_event = max(
        hint["start_sec"] + event["offset_sec"]
        for hint in hints
        for event in hint["events"]
    )
    output = np.zeros(
        int((final_event + 6.0) * args.sample_rate),
        dtype=np.float32,
    )
    rng = np.random.default_rng(args.seed)
    group_count = 0
    note_count = 0
    for hint in hints:
        for event in hint["events"]:
            group_time = (
                float(hint["start_sec"])
                + float(event["offset_sec"])
                + 2.65
                + rng.normal(0.0, 0.010)
            )
            notes = event["notes"]
            # Notes sharing a GP beat are played as a compact partial strum;
            # single-note beats remain genuine arpeggio picking.
            spacing = rng.uniform(0.006, 0.012) if len(notes) > 1 else 0.0
            velocity = float(np.clip(rng.normal(0.72, 0.09), 0.48, 0.92))
            for note_index, note in enumerate(notes):
                start = int(
                    (
                        group_time
                        + note_index * spacing
                        + rng.normal(0.0, 0.0015)
                    )
                    * args.sample_rate
                )
                add_plucked_string(
                    output,
                    start,
                    int(note["midi"]),
                    velocity * (1.0 - 0.04 * note_index),
                    args.sample_rate,
                    rng,
                )
                note_count += 1
            group_count += 1

    target_peak, noise_rms = read_reference_stats(args.reference_wav)
    output += rng.normal(
        0.0,
        noise_rms * 0.32,
        len(output),
    ).astype(np.float32)
    output *= target_peak / max(float(np.max(np.abs(output))), 1.0e-6)
    output = np.tanh(output * 1.08) / math.tanh(1.08)

    args.output_wav.parent.mkdir(parents=True, exist_ok=True)
    pcm = np.round(np.clip(output, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(str(args.output_wav), "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(args.sample_rate)
        destination.writeframes(pcm.tobytes())
    print(f"output={args.output_wav.resolve()}")
    print(f"duration_seconds={len(output) / args.sample_rate:.3f}")
    print(f"note_groups={group_count}")
    print(f"notes={note_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
