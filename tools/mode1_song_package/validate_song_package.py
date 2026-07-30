#!/usr/bin/env python3
"""Validate timing invariants for a reusable Mode 1 song package."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def first_vocal_start(package: dict) -> float:
    phrases = package.get("micro_phrases") or package.get("phrases") or []
    if not phrases:
        raise ValueError("package has no vocal phrases")
    first = phrases[0]
    return float(first.get("score", first.get("source", {}))["start_sec"])


def source_timeline(source: dict) -> list[dict]:
    return source.get("chord_timeline", source.get("chords", []))


def validate(package: dict, source: dict | None) -> dict:
    errors: list[str] = []
    warnings: list[str] = []
    timeline = package.get("chord_timeline", [])
    if not timeline:
        errors.append("package chord_timeline is empty")
        return {"valid": False, "errors": errors, "warnings": warnings}

    starts = [float(event["start_sec"]) for event in timeline]
    if starts != sorted(starts):
        errors.append("package chord_timeline is not sorted")

    vocal_start = first_vocal_start(package)
    leading = [event for event in timeline if float(event["start_sec"]) < vocal_start]
    entry_policy = package.get("entry_policy", {})
    no_intro = (
        entry_policy == "no_intro"
        or (
            isinstance(entry_policy, dict)
            and entry_policy.get("mode") == "no_intro"
        )
    )
    if not leading and not no_intro:
        errors.append(
            "first chord equals/follows first vocal but entry_policy=no_intro "
            "is not declared"
        )

    intro_sections = [
        section
        for section in package.get("sections", [])
        if section.get("type") == "instrumental_intro"
    ]
    if leading:
        if not intro_sections:
            errors.append("leading chord events exist but intro section is missing")
        else:
            intro = intro_sections[0]
            if intro.get("vocal_allowed") is not False:
                errors.append("intro section must set vocal_allowed=false")
            if abs(float(intro["end_sec"]) - vocal_start) > 1.0e-4:
                errors.append("intro section end does not equal first vocal start")

    expected_leading: list[dict] = []
    if source is not None:
        expected_leading = [
            event
            for event in source_timeline(source)
            if float(event["start_sec"]) < vocal_start
        ]
        actual_signature = [
            (round(float(event["start_sec"]), 6), str(event.get("chord", "")))
            for event in leading
        ]
        expected_signature = [
            (round(float(event["start_sec"]), 6), str(event.get("chord", "")))
            for event in expected_leading
        ]
        if actual_signature != expected_signature:
            errors.append(
                "final package does not preserve source chord events before "
                "the first vocal"
            )

    phrases = package.get("micro_phrases") or package.get("phrases") or []
    anchor_count = 0
    for phrase in phrases:
        sync = phrase.get("vocal", {}).get("sync")
        if sync is None:
            continue
        anchor_count += 1
        audible = float(sync.get("audible_onset_sec", 0.0))
        if not 0.0 <= audible <= 0.250:
            errors.append(
                f"{phrase.get('phrase_id', '?')} audible onset is outside "
                "the supported 0..250 ms range"
            )
        if "vowel_onset_sec" in sync:
            vowel = float(sync["vowel_onset_sec"])
            if not 0.0 <= vowel <= 0.250:
                errors.append(
                    f"{phrase.get('phrase_id', '?')} vowel onset is outside "
                    "the supported 0..250 ms range"
                )

    return {
        "valid": not errors,
        "errors": errors,
        "warnings": warnings,
        "metrics": {
            "first_chord_sec": starts[0],
            "first_vocal_sec": vocal_start,
            "leading_chord_events": len(leading),
            "source_leading_chord_events": len(expected_leading),
            "intro_sections": len(intro_sections),
            "vocal_anchor_phrases": anchor_count,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--source-chords", type=Path)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    result = validate(
        load_json(args.package),
        load_json(args.source_chords) if args.source_chords else None,
    )
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered + "\n", encoding="utf-8")
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
