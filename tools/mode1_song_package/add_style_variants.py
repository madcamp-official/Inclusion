"""Attach converted expression-strength variants to an existing song package."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import soundfile as sf

from build_song_package import cut_phrase


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    package_path = args.package.resolve()
    package_dir = package_path.parent
    package = json.loads(package_path.read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    default_strength = int(manifest.get("default_strength", 25))
    variants = []

    for variant in manifest["variants"]:
        strength = int(variant["strength"])
        converted_path = Path(variant["converted_vocal"])
        audio, sample_rate = sf.read(converted_path, always_2d=False)
        variants.append({
            "strength": strength,
            "converted_vocal": str(converted_path.resolve()),
        })

        for collection_name, default_directory, margin in (
            ("phrases", "vocals", 0.12),
            ("micro_phrases", "micro_vocals", 0.08),
        ):
            directory_name = f"{default_directory}_style_{strength:03d}"
            output_dir = package_dir / directory_name
            output_dir.mkdir(parents=True, exist_ok=True)
            for phrase in package.get(collection_name, []):
                filename = phrase["vocal"]["file"]
                metadata = cut_phrase(
                    audio,
                    sample_rate,
                    float(phrase["source"]["start_sec"]),
                    float(phrase["source"]["end_sec"]),
                    output_dir / filename,
                    margin,
                )
                metadata["directory"] = directory_name
                phrase.setdefault("vocal_variants", {})[str(strength)] = metadata
                if strength == default_strength:
                    phrase["vocal"] = metadata

    package["schema_version"] = max(3, int(package.get("schema_version", 1)))
    package["expression_style"] = {
        "label": "original_expression",
        "default_strength": default_strength,
        "available_strengths": [item["strength"] for item in variants],
        "variants": variants,
    }
    package_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(package_path)


if __name__ == "__main__":
    main()
