"""Compare a recorded guitar take with a Mode 1 song package timeline."""

from __future__ import annotations

import argparse
import csv
import json
import math
import wave
from pathlib import Path

import numpy as np


def read_pcm_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as stream:
        channels = stream.getnchannels()
        sample_width = stream.getsampwidth()
        sample_rate = stream.getframerate()
        frames = stream.getnframes()
        raw = stream.readframes(frames)

    if sample_width == 3:
        packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        values = (
            packed[:, 0].astype(np.int32)
            | (packed[:, 1].astype(np.int32) << 8)
            | (packed[:, 2].astype(np.int32) << 16)
        )
        values = np.where(values & 0x800000, values - 0x1000000, values)
        audio = values.astype(np.float32) / 8388608.0
    elif sample_width == 2:
        audio = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif sample_width == 4:
        audio = (
            np.frombuffer(raw, dtype="<i4").astype(np.float32)
            / 2147483648.0
        )
    else:
        raise ValueError(f"Unsupported PCM width: {sample_width} bytes")

    if channels > 1:
        audio = audio.reshape(-1, channels)[:, 0]
    return audio, sample_rate


def emulate_app_onsets(
    audio: np.ndarray,
    sample_rate: int,
    block_size: int,
) -> list[float]:
    onset_ratio = 1.6
    attack_rise_ratio = 1.22
    minimum_onset_rms = 0.01
    refractory_samples = 0.280 * sample_rate

    samples_since_onset = float(sample_rate)
    energy_baseline = 1.0e-4
    previous_rms = 0.0
    onsets: list[float] = []

    for start in range(0, len(audio), block_size):
        block = audio[start : start + block_size]
        if len(block) == 0:
            break
        current_rms = float(np.sqrt(np.mean(block.astype(np.float64) ** 2)))
        samples_since_onset += len(block)
        refractory_finished = samples_since_onset >= refractory_samples
        has_fresh_attack = (
            previous_rms <= 1.0e-5
            or current_rms >= previous_rms * attack_rise_ratio
        )
        onset = (
            refractory_finished
            and has_fresh_attack
            and current_rms >= minimum_onset_rms
            and current_rms >= energy_baseline * onset_ratio
        )

        coefficient = 0.01 if current_rms > energy_baseline else 0.08
        energy_baseline += coefficient * (current_rms - energy_baseline)
        energy_baseline = max(energy_baseline, 1.0e-4)
        if onset:
            samples_since_onset = 0.0
            onsets.append((start + len(block)) / sample_rate)
        previous_rms = current_rms
    return onsets


def simulate_scheduler(
    onsets: list[float],
    events: list[dict],
    early_window: float = 0.18,
) -> list[dict]:
    if not onsets or not events:
        return []

    accepted: list[dict] = []
    song_time = 0.0
    previous_real = onsets[0]
    current_event = -1
    next_event = 0

    for onset in onsets:
        if current_event >= 0:
            song_time += onset - previous_real
            if next_event < len(events):
                song_time = min(
                    song_time,
                    float(events[next_event]["start_sec"]) - 1.0e-4,
                )
        previous_real = onset

        if next_event >= len(events):
            break
        target = float(events[next_event]["start_sec"])
        can_advance = (
            current_event < 0 or song_time >= target - early_window
        )
        if not can_advance:
            continue

        current_event = next_event
        song_time = max(song_time, target)
        accepted.append(
            {
                "event_index": current_event,
                "expected_sec": target,
                "actual_sec": onset,
                "chord": events[current_event]["chord"],
            }
        )
        next_event += 1
    return accepted


def timing_rows(accepted: list[dict]) -> list[dict]:
    if not accepted:
        return []
    first = accepted[0]
    rows = []
    for item in accepted:
        relative_error = (
            item["actual_sec"]
            - first["actual_sec"]
            - (item["expected_sec"] - first["expected_sec"])
        )
        rows.append({**item, "relative_error_sec": relative_error})
    return rows


def line_sync_rows(
    package: dict,
    accepted: list[dict],
    vocal_lead_seconds: float,
) -> list[dict]:
    if len(accepted) < 2:
        return []

    rows: list[dict] = []
    lines = package.get("phrases", [])
    for phrase in lines:
        source_start = float(phrase["source"]["start_sec"])
        effective_start = source_start - vocal_lead_seconds
        pair_index = -1
        for index in range(len(accepted) - 1):
            if (
                accepted[index]["expected_sec"]
                <= effective_start
                < accepted[index + 1]["expected_sec"]
            ):
                pair_index = index
                break
        if pair_index < 0:
            continue

        left = accepted[pair_index]
        right = accepted[pair_index + 1]
        score_span = right["expected_sec"] - left["expected_sec"]
        if score_span <= 0.0:
            continue
        fraction = (effective_start - left["expected_sec"]) / score_span
        scheduler_actual = (
            left["actual_sec"] + effective_start - left["expected_sec"]
        )
        tempo_scaled_actual = left["actual_sec"] + fraction * (
            right["actual_sec"] - left["actual_sec"]
        )
        rows.append(
            {
                "phrase_id": phrase.get("phrase_id", ""),
                "lyrics": phrase.get("lyrics", ""),
                "source_start_sec": source_start,
                "scheduler_actual_sec": scheduler_actual,
                "tempo_scaled_actual_sec": tempo_scaled_actual,
                "sync_error_sec": scheduler_actual - tempo_scaled_actual,
                "preceding_chord": left["chord"],
            }
        )
    return rows


def percentile(values: list[float], amount: float) -> float:
    if not values:
        return 0.0
    return float(np.percentile(np.asarray(values), amount))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path)
    parser.add_argument("--block-size", type=int, default=128)
    parser.add_argument("--vocal-lead-ms", type=float, default=100.0)
    args = parser.parse_args()

    audio, sample_rate = read_pcm_wav(args.wav)
    package = json.loads(args.package.read_text(encoding="utf-8"))
    events = [
        event
        for event in package["chord_timeline"]
        if event.get("chord") not in {"N", "N.C."}
    ]
    onsets = emulate_app_onsets(audio, sample_rate, args.block_size)
    accepted = simulate_scheduler(onsets, events)
    chord_rows = timing_rows(accepted)
    line_rows = line_sync_rows(
        package, accepted, args.vocal_lead_ms / 1000.0
    )

    if args.output_csv:
        args.output_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.output_csv.open("w", encoding="utf-8-sig", newline="") as out:
            writer = csv.DictWriter(
                out,
                fieldnames=[
                    "event_index",
                    "expected_sec",
                    "actual_sec",
                    "relative_error_sec",
                    "chord",
                ],
            )
            writer.writeheader()
            writer.writerows(chord_rows)

    errors_ms = [row["relative_error_sec"] * 1000.0 for row in chord_rows]
    line_errors_ms = [row["sync_error_sec"] * 1000.0 for row in line_rows]
    duration = len(audio) / sample_rate
    rms = float(np.sqrt(np.mean(audio.astype(np.float64) ** 2)))
    peak = float(np.max(np.abs(audio))) if len(audio) else 0.0
    summary = {
        "wav": str(args.wav.resolve()),
        "sample_rate": sample_rate,
        "duration_sec": duration,
        "rms_dbfs": 20.0 * math.log10(max(rms, 1.0e-8)),
        "peak_dbfs": 20.0 * math.log10(max(peak, 1.0e-8)),
        "block_size": args.block_size,
        "raw_onsets": len(onsets),
        "accepted_chord_events": len(accepted),
        "first_onset_sec": onsets[0] if onsets else None,
        "last_onset_sec": onsets[-1] if onsets else None,
        "chord_relative_error_ms": {
            "median": percentile(errors_ms, 50),
            "p10": percentile(errors_ms, 10),
            "p90": percentile(errors_ms, 90),
            "last": errors_ms[-1] if errors_ms else 0.0,
        },
        "line_sync_error_ms": {
            "median": percentile(line_errors_ms, 50),
            "p10": percentile(line_errors_ms, 10),
            "p90": percentile(line_errors_ms, 90),
            "minimum": min(line_errors_ms) if line_errors_ms else 0.0,
            "maximum": max(line_errors_ms) if line_errors_ms else 0.0,
        },
        "largest_line_errors": sorted(
            line_rows,
            key=lambda row: abs(row["sync_error_sec"]),
            reverse=True,
        )[:10],
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
