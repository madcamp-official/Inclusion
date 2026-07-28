#!/usr/bin/env python3
"""Promote an attached vocal key anchor/expression to package defaults."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from chord_cleanup import transpose_chord_name


def rotate_pitch_mask(mask: int, semitones: int) -> int:
    result = 0
    for pitch_class in range(12):
        if mask & (1 << pitch_class):
            result |= 1 << ((pitch_class + semitones) % 12)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--key-shift", type=int, required=True)
    parser.add_argument("--strength", type=int, required=True)
    args = parser.parse_args()

    package = json.loads(
        args.package.read_text(encoding="utf-8"), strict=False
    )
    previous_shift = int(package.get("base_key_shift", 0))
    transpose_delta = args.key_shift - previous_shift
    key = str(args.key_shift)
    strength = str(args.strength)
    checked = 0
    for collection in ("phrases", "micro_phrases"):
        for phrase in package.get(collection, []):
            metadata = (
                phrase.get("vocal_key_variants", {})
                .get(key, {})
                .get(strength)
            )
            if metadata is None:
                raise ValueError(
                    f"Missing key {key}, strength {strength}: "
                    f"{collection}/{phrase.get('phrase_id')}"
                )
            checked += 1

    def transpose_chords(events: list[dict]) -> None:
        for event in events:
            current = str(event.get("chord", "N"))
            original = event.get("original_chord")
            if not original:
                original = transpose_chord_name(current, -previous_shift)
                event["original_chord"] = original
            event["chord"] = transpose_chord_name(
                str(original), args.key_shift
            )

    transpose_chords(package.get("chord_timeline", []))
    for collection in ("phrases", "micro_phrases"):
        for phrase in package.get(collection, []):
            transpose_chords(phrase.get("chords", []))

    for hint in package.get("tab_tracking", {}).get("chord_hints", []):
        hint["pitch_class_mask"] = rotate_pitch_mask(
            int(hint.get("pitch_class_mask", 0)), transpose_delta
        )
        hint["bass_pitch_class_mask"] = rotate_pitch_mask(
            int(hint.get("bass_pitch_class_mask", 0)), transpose_delta
        )
        for event in hint.get("events", []):
            for note in event.get("notes", []):
                if "midi" in note:
                    note["midi"] = int(note["midi"]) + transpose_delta
                if "pitch_class" in note:
                    note["pitch_class"] = (
                        int(note["pitch_class"]) + transpose_delta
                    ) % 12

    package["base_key_shift"] = args.key_shift
    package["expression_style"] = {
        "label": "single_expression_anchor",
        "default_strength": args.strength,
        "available_strengths": [args.strength],
        "variants": [{
            "strength": args.strength,
            "key_shift": args.key_shift,
        }],
    }
    key_style = package.setdefault("key_style", {})
    key_style.update({
        "label": "single_profile_key",
        "default_key_shift": args.key_shift,
        "available_key_shifts": [args.key_shift],
        "expression_strengths": [args.strength],
        "manual_key_shift_range": [-3, 3],
        "selection": "single_pre_rendered_anchor",
    })
    package.setdefault("vocal_style", {})["style_strength"] = args.strength
    package["vocal_style"]["base_key_shift"] = args.key_shift
    args.package.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "package": str(args.package.resolve()),
        "key_shift": args.key_shift,
        "strength": args.strength,
        "transposed_semitones": transpose_delta,
        "validated_references": checked,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
