#!/usr/bin/env python3
"""Promote an attached vocal key anchor/expression to package defaults."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--key-shift", type=int, required=True)
    parser.add_argument("--strength", type=int, required=True)
    args = parser.parse_args()

    package = json.loads(
        args.package.read_text(encoding="utf-8"), strict=False
    )
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
        "validated_references": checked,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
