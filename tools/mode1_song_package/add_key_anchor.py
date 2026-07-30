"""Attach one pre-rendered key anchor to an existing Mode 1 song package."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import soundfile as sf

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    package_path = args.package.resolve()
    package_dir = package_path.parent
    package = json.loads(package_path.read_text(encoding="utf-8"))
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    anchor = int(manifest["base_key_shift"])
    strengths: list[int] = []

    for variant in manifest["variants"]:
        strength = int(variant["strength"])
        converted_path = Path(variant["converted_vocal"]).resolve()
        if not converted_path.is_file():
            raise FileNotFoundError(converted_path)
        duration = sf.info(converted_path).duration
        try:
            relative_parent = converted_path.parent.relative_to(package_dir)
        except ValueError as exc:
            raise ValueError(
                "Converted key-anchor audio must be inside the song package "
                f"directory: {converted_path}"
            ) from exc
        strengths.append(strength)

        for collection_name, margin in (
            ("phrases", 0.12),
            ("micro_phrases", 0.08),
        ):
            for phrase in package.get(collection_name, []):
                start = float(phrase["source"]["start_sec"])
                end = float(phrase["source"]["end_sec"])
                metadata = {
                    "directory": relative_parent.as_posix(),
                    "file": converted_path.name,
                    "content_offset_sec": 0.0,
                    "playback_start_sec": round(start, 6),
                    "playback_end_sec": round(
                        min(duration, end + margin), 6
                    ),
                }
                phrase.setdefault("vocal_key_variants", {}).setdefault(
                    str(anchor), {}
                )[str(strength)] = metadata

    key_style = package.setdefault("key_style", {})
    anchors = {
        int(value)
        for value in key_style.get(
            "available_key_shifts",
            [int(package.get("base_key_shift", 0))],
        )
    }
    anchors.add(int(package.get("base_key_shift", 0)))
    anchors.add(anchor)
    key_style.update({
        "label": "pre_rendered_key_anchor",
        "default_key_shift": int(package.get("base_key_shift", 0)),
        "available_key_shifts": sorted(anchors),
        "expression_strengths": sorted(set(strengths)),
        "selection": "nearest_anchor_then_realtime_residual",
    })
    package["schema_version"] = max(4, int(package.get("schema_version", 1)))
    package_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "package": str(package_path),
        "anchor": anchor,
        "strengths": sorted(set(strengths)),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
