"""Promote a package built in a nested directory to its parent directory.

Audio directory references are prefixed with the candidate directory name so
the promoted JSON continues to resolve the candidate's generated WAV files.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def prefix_audio_directories(value: object, prefix: str) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if (
                key == "directory"
                and isinstance(child, str)
                and child
                and not Path(child).is_absolute()
                and not child.replace("\\", "/").startswith(prefix + "/")
            ):
                value[key] = prefix + "/" + child.replace("\\", "/")
            else:
                prefix_audio_directories(child, prefix)
    elif isinstance(value, list):
        for child in value:
            prefix_audio_directories(child, prefix)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    package = json.loads(args.candidate.read_text(encoding="utf-8"))
    prefix = args.candidate.parent.name
    prefix_audio_directories(package, prefix)

    control = package.setdefault("control_mode", {})
    control["strategy"] = "score_constrained_onset_follower"
    control["candidate_scope"] = "current_and_nearby_score_events"
    control["generic_chord_labels_are_diagnostic_only"] = True
    control["same_callback_phrase_start"] = True

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"output={args.destination.resolve()}")


if __name__ == "__main__":
    main()
