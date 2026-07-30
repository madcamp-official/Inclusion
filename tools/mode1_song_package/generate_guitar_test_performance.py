#!/usr/bin/env python3
"""Generate a deterministic, humanised full-song guitar test performance.

The signal is intentionally causal-test input, not a precomputed scheduler
trace. It renders playable guitar-like chord tones from the score, adds
realistic strum timing/velocity variation and subdivision strums, then matches
the level and low background noise of an optional real guitar recording.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import wave
from pathlib import Path

import numpy as np


NOTE_TO_PC = {
    "C": 0,
    "D": 2,
    "E": 4,
    "F": 5,
    "G": 7,
    "A": 9,
    "B": 11,
}


def note_pc(name: str) -> int:
    match = re.match(r"^([A-G])([b#]?)", name)
    if not match:
        raise ValueError(f"unsupported note: {name}")
    pc = NOTE_TO_PC[match.group(1)]
    if match.group(2) == "b":
        pc -= 1
    elif match.group(2) == "#":
        pc += 1
    return pc % 12


def chord_pitch_classes(chord: str) -> tuple[int, list[int]]:
    chord_part, _, bass_part = chord.partition("/")
    root_match = re.match(r"^([A-G](?:b|#)?)(.*)$", chord_part)
    if not root_match:
        raise ValueError(f"unsupported chord: {chord}")
    root_name, quality = root_match.groups()
    root = note_pc(root_name)
    quality_lower = quality.lower()
    if "dim" in quality_lower:
        intervals = [0, 3, 6]
    elif "sus4" in quality_lower:
        intervals = [0, 5, 7]
    elif quality_lower.startswith("m") and not quality_lower.startswith("maj"):
        intervals = [0, 3, 7, 10] if "7" in quality_lower else [0, 3, 7]
    elif "maj7" in quality_lower:
        intervals = [0, 4, 7, 11]
    elif "7" in quality_lower:
        intervals = [0, 4, 7, 10]
    else:
        intervals = [0, 4, 7]
    pcs = [(root + interval) % 12 for interval in intervals]
    bass = note_pc(bass_part) if bass_part else root
    if bass not in pcs:
        pcs.append(bass)
    return bass, pcs


def closest_note(pc: int, low: int, high: int, above: int | None = None) -> int:
    notes = [midi for midi in range(low, high + 1) if midi % 12 == pc]
    if above is not None:
        higher = [midi for midi in notes if midi > above]
        if higher:
            return higher[0]
    return notes[0]


def guitar_voicing(chord: str) -> list[int]:
    bass, pcs = chord_pitch_classes(chord)
    notes = [closest_note(bass, 40, 52)]
    ordered = []
    for octave in range(4, 7):
        for pc in pcs:
            midi = 12 * octave + pc
            if 50 <= midi <= 78:
                ordered.append(midi)
    ordered.sort()
    cursor = notes[0]
    while len(notes) < 6:
        candidates = [midi for midi in ordered if midi > cursor]
        if not candidates:
            break
        cursor = candidates[0]
        notes.append(cursor)
    return notes


def read_reference_stats(path: Path | None) -> tuple[float, float]:
    if path is None:
        return 0.62, 0.0015
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        frames = source.readframes(source.getnframes())
    if width == 2:
        raw = np.frombuffer(frames, dtype="<i2")
        scale = 32768.0
    elif width == 3:
        bytes24 = np.frombuffer(frames, dtype=np.uint8).reshape(-1, 3)
        raw = (
            bytes24[:, 0].astype(np.int32)
            | (bytes24[:, 1].astype(np.int32) << 8)
            | (bytes24[:, 2].astype(np.int32) << 16)
        )
        raw = (raw ^ 0x800000) - 0x800000
        scale = 8388608.0
    elif width == 4:
        raw = np.frombuffer(frames, dtype="<i4")
        scale = 2147483648.0
    else:
        raise ValueError("reference WAV must use 16-, 24-, or 32-bit PCM")
    mono = raw.reshape(-1, channels).mean(axis=1) / scale
    peak = float(np.max(np.abs(mono)))
    frame = 2048
    trimmed = mono[: len(mono) // frame * frame]
    frame_rms = np.sqrt(np.mean(trimmed.reshape(-1, frame) ** 2, axis=1))
    noise = float(np.percentile(frame_rms, 10))
    return min(0.72, max(0.45, peak)), min(0.004, max(0.0005, noise))


def add_plucked_string(
    output: np.ndarray,
    start: int,
    midi: int,
    amplitude: float,
    sample_rate: int,
    rng: np.random.Generator,
) -> None:
    duration = 2.8
    count = min(int(duration * sample_rate), len(output) - start)
    if count <= 0:
        return
    time = np.arange(count, dtype=np.float32) / sample_rate
    frequency = 440.0 * (2.0 ** ((midi - 69) / 12.0))
    frequency *= 2.0 ** (rng.normal(0.0, 2.5) / 1200.0)
    attack = np.minimum(1.0, time / 0.003)
    body = np.zeros(count, dtype=np.float32)
    phases = rng.uniform(0.0, 2.0 * math.pi, 5)
    for harmonic in range(1, 6):
        harmonic_decay = np.exp(
            -time * (1.7 + 0.32 * harmonic + 0.002 * frequency)
        )
        body += (
            np.sin(2.0 * math.pi * frequency * harmonic * time + phases[harmonic - 1])
            * harmonic_decay
            / (harmonic ** 1.35)
        ).astype(np.float32)
    pick_count = min(count, int(0.018 * sample_rate))
    pick = np.zeros(count, dtype=np.float32)
    pick[:pick_count] = (
        rng.normal(0.0, 1.0, pick_count)
        * np.exp(-np.arange(pick_count) / (0.0035 * sample_rate))
    )
    signal = amplitude * attack * (0.82 * body + 0.12 * pick)
    output[start : start + count] += signal.astype(np.float32)


def add_strum(
    output: np.ndarray,
    time_seconds: float,
    chord: str,
    velocity: float,
    downstroke: bool,
    sample_rate: int,
    rng: np.random.Generator,
) -> None:
    notes = guitar_voicing(chord)
    if not downstroke:
        notes = list(reversed(notes))
    spacing = rng.uniform(0.007, 0.013)
    for string_index, midi in enumerate(notes):
        start = int(
            (time_seconds + string_index * spacing + rng.normal(0.0, 0.0015))
            * sample_rate
        )
        if start >= 0:
            string_balance = 1.0 - 0.055 * string_index
            add_plucked_string(
                output,
                start,
                midi,
                velocity * string_balance,
                sample_rate,
                rng,
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("song_package", type=Path)
    parser.add_argument("output_wav", type=Path)
    parser.add_argument("--reference-wav", type=Path)
    parser.add_argument("--seed", type=int, default=1031)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument(
        "--no-subdivisions",
        action="store_true",
        help="Render exactly one main strum per score event.",
    )
    args = parser.parse_args()

    package = json.loads(args.song_package.read_text(encoding="utf-8"))
    playable = [
        event
        for event in package["chord_timeline"]
        if event.get("chord", "").strip().upper() not in {"", "N", "N.C."}
    ]
    rng = np.random.default_rng(args.seed)

    # Integrate a slow, natural tempo curve. The performer averages close to
    # score tempo but breathes by a few percent across musical sections.
    performance_times: list[float] = []
    previous_score = float(playable[0]["start_sec"])
    previous_real = 2.65
    timing_wander = 0.0
    for event_index, event in enumerate(playable):
        score_time = float(event["start_sec"])
        midpoint = (previous_score + score_time) * 0.5
        tempo_scale = (
            0.985
            + 0.026 * math.sin(midpoint / 18.0)
            + 0.014 * math.sin(midpoint / 6.7 + 0.8)
        )
        timing_wander = 0.72 * timing_wander + rng.normal(0.0, 0.010)
        real_time = previous_real + (score_time - previous_score) / tempo_scale
        if event_index > 0:
            real_time += timing_wander
        performance_times.append(real_time)
        previous_score = score_time
        previous_real = real_time

    duration = performance_times[-1] + 6.0
    output = np.zeros(int(duration * args.sample_rate), dtype=np.float32)
    main_strums = 0
    subdivision_strums = 0
    for index, (event, event_time) in enumerate(zip(playable, performance_times)):
        velocity = float(np.clip(rng.normal(0.88, 0.10), 0.62, 1.05))
        add_strum(
            output,
            event_time,
            event["chord"],
            velocity,
            True,
            args.sample_rate,
            rng,
        )
        main_strums += 1

        if args.no_subdivisions or index + 1 >= len(playable):
            continue
        interval = performance_times[index + 1] - event_time
        beat = 60.0 / float(package.get("score_bpm", 103.1))
        repeat_count = max(0, int(interval / beat) - 1)
        for repeat in range(repeat_count):
            repeat_time = (
                event_time
                + beat * (repeat + 1)
                + rng.normal(0.0, 0.018)
            )
            if repeat_time >= performance_times[index + 1] - 0.12:
                continue
            add_strum(
                output,
                repeat_time,
                event["chord"],
                float(np.clip(rng.normal(0.48, 0.07), 0.32, 0.62)),
                repeat % 2 == 1,
                args.sample_rate,
                rng,
            )
            subdivision_strums += 1

    target_peak, noise_rms = read_reference_stats(args.reference_wav)
    output += rng.normal(0.0, noise_rms * 0.32, len(output)).astype(np.float32)
    current_peak = float(np.max(np.abs(output)))
    output *= target_peak / max(current_peak, 1.0e-6)
    output = np.tanh(output * 1.08) / math.tanh(1.08)

    args.output_wav.parent.mkdir(parents=True, exist_ok=True)
    pcm = np.round(np.clip(output, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(str(args.output_wav), "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(args.sample_rate)
        destination.writeframes(pcm.tobytes())

    manifest = {
        "seed": args.seed,
        "duration_seconds": duration,
        "main_score_strums": main_strums,
        "subdivision_strums": subdivision_strums,
        "target_peak_from_reference": target_peak,
        "reference_noise_rms": noise_rms,
        "first_performance_chord_seconds": performance_times[0],
        "last_performance_chord_seconds": performance_times[-1],
    }
    args.output_wav.with_suffix(".json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
