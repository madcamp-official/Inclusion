"""Create deterministic offline Mode 1 vocal-following comparison renders."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf


NOTE_TO_PC = {
    "C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3,
    "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
    "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11,
}
PC_TO_NAME = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def chord_root(chord: str) -> int | None:
    if not chord or chord == "N":
        return None
    root = chord[0]
    if len(chord) > 1 and chord[1] in "#b":
        root += chord[1]
    return NOTE_TO_PC.get(root)


def expected_root(phrases: list[dict], phrase_index: int) -> int | None:
    for index in range(phrase_index, -1, -1):
        for event in phrases[index].get("chords", []):
            root = chord_root(event["chord"])
            if root is not None:
                return root
    return None


def signed_delta(actual: int, expected: int) -> int:
    delta = actual - expected
    while delta > 6:
        delta -= 12
    while delta < -6:
        delta += 12
    return delta


def load_vocal(phrase: dict, package_dir: Path, sample_rate: int) -> np.ndarray:
    vocal = phrase["vocal"]
    path = package_dir / vocal.get("directory", "vocals") / vocal["file"]
    audio, source_sr = sf.read(path, always_2d=True, dtype="float32")
    audio = audio.mean(axis=1)
    if source_sr != sample_rate:
        audio = librosa.resample(audio, orig_sr=source_sr, target_sr=sample_rate)
    offset = round(float(vocal.get("content_offset_sec", 0.0)) * sample_rate)
    return audio[min(offset, len(audio)) :]


def render_scenario(
    phrases: list[dict],
    package_dir: Path,
    sample_rate: int,
    duration_sec: float,
    transpose: int,
    mistake_index: int | None,
    cache: dict[tuple[str, int], np.ndarray],
) -> tuple[np.ndarray, list[dict]]:
    first_start = float(phrases[0]["score"]["start_sec"])
    output = np.zeros(round(duration_sec * sample_rate), dtype=np.float32)
    stable_shift = 0
    pending_shift = 0
    pending_count = 0
    events = []

    for index, phrase in enumerate(phrases):
        time_sec = float(phrase["score"]["start_sec"]) - first_start
        if time_sec >= duration_sec:
            break
        expected = expected_root(phrases, index)
        if expected is None:
            continue

        played_delta = transpose + (5 if mistake_index == index else 0)
        detected = (expected + played_delta) % 12
        difference = signed_delta(detected, expected)
        if difference == pending_shift:
            pending_count += 1
        else:
            pending_shift = difference
            pending_count = 1
        if pending_count >= 2:
            stable_shift = max(-6, min(6, difference))

        key = (phrase["vocal"]["file"], stable_shift)
        if key not in cache:
            clip = load_vocal(phrase, package_dir, sample_rate)
            if stable_shift:
                clip = librosa.effects.pitch_shift(
                    clip, sr=sample_rate, n_steps=stable_shift, res_type="soxr_hq"
                )
            cache[key] = clip.astype(np.float32)
        clip = cache[key]
        start = round(time_sec * sample_rate)
        count = min(len(clip), len(output) - start)
        if count > 0:
            output[start : start + count] += clip[:count]

        events.append({
            "time_sec": round(time_sec, 6),
            "phrase_index": index,
            "phrase_id": phrase["phrase_id"],
            "lyrics": phrase.get("lyrics", ""),
            "expected_chord_root": PC_TO_NAME[expected],
            "simulated_chord_root": PC_TO_NAME[detected],
            "detected_delta": difference,
            "confirmed_pitch_shift": stable_shift,
            "injected_mistake": mistake_index == index,
        })

    peak = float(np.max(np.abs(output))) if len(output) else 0.0
    if peak > 0.98:
        output *= 0.98 / peak
    return output, events


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--sample-rate", type=int, default=40_000)
    args = parser.parse_args()

    package_path = args.package.resolve()
    package_dir = package_path.parent
    output_dir = (args.output_dir or package_dir / "simulation").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    package = json.loads(package_path.read_text(encoding="utf-8"))
    phrases = package.get("micro_phrases") or package["phrases"]

    scenarios = (
        ("normal", 0, None),
        ("transpose_plus2", 2, None),
        ("single_mistake", 0, 8),
    )
    cache: dict[tuple[str, int], np.ndarray] = {}
    rendered = []
    log = {
        "package": str(package_path),
        "sample_rate": args.sample_rate,
        "scenario_duration_sec": args.duration,
        "confirmation_rule": "same detected pitch delta twice consecutively",
        "scenarios": {},
    }
    for name, transpose, mistake_index in scenarios:
        audio, events = render_scenario(
            phrases, package_dir, args.sample_rate, args.duration,
            transpose, mistake_index, cache
        )
        path = output_dir / f"mode1_{name}.wav"
        sf.write(path, audio, args.sample_rate, subtype="PCM_24")
        rendered.append(audio)
        log["scenarios"][name] = {
            "transpose": transpose,
            "mistake_phrase_index": mistake_index,
            "output": str(path),
            "events": events,
        }

    silence = np.zeros(args.sample_rate, dtype=np.float32)
    combined = np.concatenate(
        [part for audio in rendered for part in (audio, silence)]
    )[:-len(silence)]
    combined_path = output_dir / "mode1_simulation.wav"
    sf.write(combined_path, combined, args.sample_rate, subtype="PCM_24")
    log["combined_output"] = str(combined_path)
    log_path = output_dir / "mode1_event_log.json"
    log_path.write_text(
        json.dumps(log, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        "combined_output": str(combined_path),
        "event_log": str(log_path),
        "scenario_outputs": [
            log["scenarios"][name]["output"] for name, _, _ in scenarios
        ],
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
