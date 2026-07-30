#!/usr/bin/env python3
"""Attach one already-rendered expression bank to a package copy."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"), strict=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--variant-source", type=Path, required=True)
    parser.add_argument("--strength", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    package = load(args.package)
    donor = load(args.variant_source)
    key = str(args.strength)

    for collection in ("phrases", "micro_phrases"):
        destination_items = package.get(collection, [])
        donor_by_id = {
            item.get("phrase_id"): item for item in donor.get(collection, [])
        }
        for item in destination_items:
            source = donor_by_id.get(item.get("phrase_id"))
            metadata = (source or {}).get("vocal_variants", {}).get(key)
            if not metadata:
                raise ValueError(
                    f"Missing cached strength {key} for "
                    f"{collection}/{item.get('phrase_id')}"
                )
            audio = args.package.parent / metadata["directory"] / metadata["file"]
            if not audio.is_file():
                raise FileNotFoundError(audio)
            item.setdefault("vocal_variants", {})[key] = metadata

    package["expression_style"] = {
        "label": "cached_single_expression",
        "default_strength": args.strength,
        "available_strengths": [args.strength],
        "variants": [
            item for item in donor.get("expression_style", {}).get("variants", [])
            if int(item.get("strength", -1)) == args.strength
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
