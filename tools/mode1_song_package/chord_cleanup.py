"""Normalize noisy chord labels and apply them to a Mode 1 song package."""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


ROOT_PATTERN = re.compile(r"^([A-Ga-g])([#b]?)(.*)$")
ROOT_TO_PC = {
    "C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3,
    "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
    "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11,
}
PC_TO_ROOT = ("C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B")


def normalize_chord_name(name: str) -> str:
    chord = name.strip().replace("♭", "b").replace("♯", "#")
    if not chord or chord.upper() in {"N", "N.C", "N.C."}:
        return "N"

    explicit = {
        "Dbad": "Db",
        "Ab/": "Ab",
        "DbM": "Db",
        "DbM7": "Dbmaj7",
        "DM7": "Dmaj7",
        "Bb7su": "Bb7sus4",
        "B7sus": "B7sus4",
        "Ebsus": "Ebsus4",
        "Dsus": "Dsus4",
    }
    if chord in explicit:
        return explicit[chord]
    if chord.endswith("/"):
        chord = chord[:-1]

    match = ROOT_PATTERN.match(chord)
    if match is None:
        return chord
    letter, accidental, suffix = match.groups()
    root = letter.upper() + accidental
    if "/" in suffix:
        quality, bass = suffix.split("/", 1)
        bass_match = ROOT_PATTERN.match(bass)
        if bass_match is not None:
            bass = bass_match.group(1).upper() + bass_match.group(2)
        suffix = quality + (f"/{bass}" if bass else "")
    return root + suffix


def transpose_chord_name(name: str, semitones: int) -> str:
    chord = normalize_chord_name(name)
    if chord == "N":
        return chord
    match = ROOT_PATTERN.match(chord)
    if match is None:
        return chord
    letter, accidental, suffix = match.groups()
    root = letter.upper() + accidental
    if root not in ROOT_TO_PC:
        return chord
    transposed = PC_TO_ROOT[(ROOT_TO_PC[root] + semitones) % 12]
    if "/" in suffix:
        quality, bass = suffix.split("/", 1)
        bass_match = ROOT_PATTERN.match(bass)
        if bass_match is not None:
            bass_root = bass_match.group(1).upper() + bass_match.group(2)
            if bass_root in ROOT_TO_PC:
                bass = PC_TO_ROOT[(ROOT_TO_PC[bass_root] + semitones) % 12]
        suffix = quality + "/" + bass
    return transposed + suffix


def clean_chord_events(
    events: list[dict],
    minimum_event_seconds: float = 0.35,
    preserve_repeated_chords: bool = False,
) -> list[dict]:
    normalized = [
        {
            "start_sec": float(event["start_sec"]),
            "raw_chord": event.get("raw_chord", event.get("chord", "")),
            "chord": normalize_chord_name(
                event.get("raw_chord", event.get("chord", ""))
            ),
        }
        for event in events
    ]
    normalized.sort(key=lambda event: event["start_sec"])

    # Half-beat changes in Bansanka are ~0.58 s, so this only removes faster
    # detector flicker sandwiched between identical chords.
    filtered: list[dict] = []
    for index, event in enumerate(normalized):
        if 0 < index < len(normalized) - 1:
            duration = normalized[index + 1]["start_sec"] - event["start_sec"]
            if (
                duration < minimum_event_seconds
                and normalized[index - 1]["chord"]
                == normalized[index + 1]["chord"]
            ):
                continue
        filtered.append(event)

    merged: list[dict] = []
    for event in filtered:
        if (
            not preserve_repeated_chords
            and merged
            and merged[-1]["chord"] == event["chord"]
        ):
            continue
        merged.append(event)
    return merged


def chords_for_span(
    chord_events: list[dict],
    start_sec: float,
    end_sec: float,
) -> list[dict]:
    active = None
    inside = []
    for event in chord_events:
        if event["start_sec"] <= start_sec:
            active = event
        if start_sec <= event["start_sec"] < end_sec:
            inside.append(event)
    if active is not None and (
        not inside or inside[0]["start_sec"] > start_sec + 1e-6
    ):
        return [active, *inside]
    return inside


def clean_package(package: dict) -> tuple[dict, dict]:
    old_events = package["chord_timeline"]
    new_events = clean_chord_events(old_events)
    package["chord_timeline"] = new_events

    updated_phrases = 0
    for collection_name in ("phrases", "micro_phrases"):
        for phrase in package.get(collection_name, []):
            source = phrase["source"]
            phrase["chords"] = chords_for_span(
                new_events,
                float(source["start_sec"]),
                float(source["end_sec"]),
            )
            updated_phrases += 1

    report = {
        "events_before": len(old_events),
        "events_after": len(new_events),
        "removed_events": len(old_events) - len(new_events),
        "updated_phrases": updated_phrases,
        "canonical_chords": sorted({event["chord"] for event in new_events}),
    }
    package["chord_cleanup"] = {
        "method": "canonicalize_then_merge",
        "minimum_noise_event_sec": 0.35,
        **report,
    }
    return package, report


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--in-place", action="store_true")
    args = parser.parse_args()
    if args.in_place and args.output is not None:
        parser.error("--in-place and --output cannot be used together")

    package_path = args.package.resolve()
    package = json.loads(package_path.read_text(encoding="utf-8"))
    package, report = clean_package(package)

    if args.in_place:
        backup = package_path.with_name(package_path.stem + ".raw.json")
        if not backup.exists():
            shutil.copy2(package_path, backup)
        output = package_path
    else:
        output = (
            args.output.resolve()
            if args.output
            else package_path.with_name(package_path.stem + ".cleaned.json")
        )
    output.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"output": str(output), **report}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
