#!/usr/bin/env python3
"""Pitch-shift an audio fixture while preserving its duration."""

from __future__ import annotations

import argparse
from pathlib import Path

import librosa
import soundfile as sf


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--semitones", type=float, required=True)
    args = parser.parse_args()

    audio, sample_rate = sf.read(
        args.input, always_2d=True, dtype="float32"
    )
    shifted = [
        librosa.effects.pitch_shift(
            audio[:, channel],
            sr=sample_rate,
            n_steps=args.semitones,
            res_type="soxr_hq",
        )
        for channel in range(audio.shape[1])
    ]
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(output, list(zip(*shifted)), sample_rate, subtype="PCM_24")
    print(output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
