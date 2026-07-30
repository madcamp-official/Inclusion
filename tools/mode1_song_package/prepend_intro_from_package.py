"""Prepend silent-intro chord events from an existing Mode 1 package.

This keeps a phrase-anchored package responsive while retaining chord events
that must be played before the first vocal phrase (for example, a four-bar
instrumental intro).
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--intro-package", type=Path, required=True)
    parser.add_argument("--before-sec", type=float, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    package = json.loads(args.package.read_text(encoding="utf-8"))
    intro_package = json.loads(args.intro_package.read_text(encoding="utf-8"))

    intro_timeline = intro_package.get(
        "chord_timeline",
        intro_package.get("chords", []),
    )
    leading_chords = [
        copy.deepcopy(event)
        for event in intro_timeline
        if float(event.get("start_sec", 0.0)) < args.before_sec
    ]
    if not leading_chords:
        raise SystemExit("No leading chord events matched --before-sec")

    phrases = package.get("micro_phrases", package.get("phrases", []))
    if not phrases:
        raise SystemExit("Package has no vocal phrases")
    first_vocal_chord_sec = float(
        phrases[0].get("score", phrases[0].get("source", {}))["start_sec"]
    )
    leading_chords = [
        event
        for event in leading_chords
        if float(event.get("start_sec", 0.0)) < first_vocal_chord_sec
    ]
    current_timeline = package["chord_timeline"]
    current_leading_count = sum(
        float(event.get("start_sec", 0.0)) < first_vocal_chord_sec
        for event in current_timeline
    )
    offset = len(leading_chords)
    offset_delta = offset - current_leading_count
    package["chord_timeline"] = leading_chords + [
        event
        for event in current_timeline
        if float(event.get("start_sec", 0.0)) >= first_vocal_chord_sec
    ]
    package["sections"] = [
        {
            "id": "intro_1",
            "start_sec": round(
                float(package["chord_timeline"][0]["start_sec"]),
                6,
            ),
            "end_sec": round(first_vocal_chord_sec, 6),
            "type": "instrumental_intro",
            "vocal_allowed": False,
            "clock_policy": "lock_from_guitar",
            "entry_policy": "predict_first_vowel",
        },
        {
            "id": "vocal_1",
            "start_sec": round(first_vocal_chord_sec, 6),
            "end_sec": round(
                max(
                    float(phrase.get("source", {}).get("end_sec", 0.0))
                    for phrase in package.get(
                        "micro_phrases",
                        package.get("phrases", []),
                    )
                ),
                6,
            ),
            "type": "vocal",
            "vocal_allowed": True,
        },
    ]

    tab_tracking = package.get("tab_tracking")
    intro_tab_tracking = intro_package.get("tab_tracking", {})
    if isinstance(tab_tracking, dict):
        leading_hints = [
            copy.deepcopy(hint)
            for hint in intro_tab_tracking.get("chord_hints", [])
            if int(hint.get("chord_event_index", -1)) < offset
        ]
        current_hints = copy.deepcopy(tab_tracking.get("chord_hints", []))
        for hint in current_hints:
            hint["chord_event_index"] = (
                int(hint.get("chord_event_index", 0)) + offset_delta
            )
        existing_leading_hints = [
            hint
            for hint in current_hints
            if int(hint.get("chord_event_index", -1)) < offset
        ]
        tab_tracking["chord_hints"] = (
            leading_hints + existing_leading_hints + [
                hint
                for hint in current_hints
                if int(hint.get("chord_event_index", -1)) >= offset
            ]
        )

    score_source = package.setdefault("score_source", {})
    score_source["intro_chord_source"] = str(args.intro_package.resolve())
    score_source["prepended_intro_chord_events"] = offset

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"prepended_intro_chords={offset}")
    print(f"total_chords={len(package['chord_timeline'])}")
    print(f"output={args.output.resolve()}")


if __name__ == "__main__":
    main()
