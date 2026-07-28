"""Render a trained RVC model into every Mode 1 voice bank and publish it.

The existing prepared RVC inputs are reused for the base key and every
pre-rendered key anchor. Outputs are written to a model-specific directory.
The live song_package.json is replaced only after every conversion, resample,
slice, and reference validation succeeds.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from chord_cleanup import transpose_chord_name


SCRIPT_DIR = Path(__file__).resolve().parent


def run(command: list[str], label: str) -> None:
    print(f"[{label}] running...", flush=True)
    subprocess.run(command, check=True)
    print(f"[{label}] done", flush=True)


def safe_tag(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_-]", "_", value).strip("_")
    if not result:
        raise ValueError("Experiment name does not contain a usable tag")
    return result


def anchor_directory_name(shift: int) -> str:
    return f"m{abs(shift):03d}" if shift < 0 else f"p{shift:03d}"


def resolve_remote_index(host: str, experiment: str) -> str:
    remote = (
        f"cd /root/rvc-webui && "
        f"ls -t logs/{safe_tag(experiment)}/added_*.index | head -n 1"
    )
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", host, remote],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if not result:
        raise RuntimeError(
            f"No trained index found for experiment {experiment}"
        )
    return result


def copy_manifest(source: Path, destination_dir: Path) -> Path:
    if not source.is_file():
        raise FileNotFoundError(source)
    destination_dir.mkdir(parents=True, exist_ok=True)
    destination = destination_dir / "style_variants.json"
    manifest = json.loads(source.read_text(encoding="utf-8"))
    # Discard old conversion paths while retaining prepared inputs and plans.
    for variant in manifest["variants"]:
        variant.pop("converted_vocal", None)
    destination.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return destination


def copy_single_strength_manifest(
    source: Path,
    destination_dir: Path,
    strength: int = 100,
) -> Path:
    destination = copy_manifest(source, destination_dir)
    manifest = json.loads(destination.read_text(encoding="utf-8"))
    selected = [
        variant
        for variant in manifest["variants"]
        if int(variant["strength"]) == strength
    ]
    if len(selected) != 1:
        raise ValueError(
            f"{source} does not contain exactly one strength {strength} variant"
        )
    selected[0].pop("rvc_pitch_shift", None)
    manifest["variants"] = selected
    manifest["default_strength"] = strength
    manifest["render_mode"] = "prepared_anchor_single_strength"
    destination.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return destination


def convert_manifest(
    manifest: Path,
    host: str,
    model: str,
    index: str,
    job_tag: str,
) -> None:
    run(
        [
            sys.executable,
            str(SCRIPT_DIR / "run_rvc_variants_remote.py"),
            "--manifest",
            str(manifest),
            "--host",
            host,
            "--model",
            model,
            "--index",
            index,
            "--output-dir",
            str(manifest.parent),
            "--job-tag",
            job_tag,
        ],
        f"RVC 변환 {manifest.parent.name}",
    )
    run(
        [
            sys.executable,
            str(SCRIPT_DIR / "resample_manifest_audio.py"),
            "--manifest",
            str(manifest),
            "--sample-rate",
            "48000",
        ],
        f"48 kHz 변환 {manifest.parent.name}",
    )


def validate_package(package_path: Path) -> dict[str, int]:
    package = json.loads(package_path.read_text(encoding="utf-8"))
    package_dir = package_path.parent
    checked_default = 0
    checked_key = 0
    for collection_name in ("phrases", "micro_phrases"):
        for phrase in package.get(collection_name, []):
            vocal = phrase["vocal"]
            default_file = (
                package_dir
                / vocal.get("directory", collection_name)
                / vocal["file"]
            )
            if not default_file.is_file() or default_file.stat().st_size == 0:
                raise FileNotFoundError(default_file)
            checked_default += 1

            for strengths in phrase.get("vocal_key_variants", {}).values():
                for metadata in strengths.values():
                    key_file = (
                        package_dir
                        / metadata["directory"]
                        / metadata["file"]
                    )
                    if not key_file.is_file() or key_file.stat().st_size == 0:
                        raise FileNotFoundError(key_file)
                    checked_key += 1
    if checked_default == 0:
        raise RuntimeError("Refreshed package contains no playable vocals")
    return {
        "default_vocal_references": checked_default,
        "key_anchor_references": checked_key,
    }

def configure_single_best_render(
    package_path: Path,
    manifest_path: Path,
) -> int:
    package = json.loads(package_path.read_text(encoding="utf-8"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if len(manifest["variants"]) != 1:
        raise ValueError("Single-render mode requires exactly one variant")
    variant = manifest["variants"][0]
    plan = json.loads(Path(variant["style_plan"]).read_text(encoding="utf-8"))
    new_shift = int(manifest["base_key_shift"])
    old_shift = int(package.get("base_key_shift", 0))

    def retune(events: list[dict]) -> None:
        for event in events:
            current = str(event.get("chord", "N"))
            original = event.get("original_chord")
            if not original:
                original = transpose_chord_name(current, -old_shift)
                event["original_chord"] = original
            event["chord"] = transpose_chord_name(str(original), new_shift)

    retune(package.get("chord_timeline", []))
    for collection_name in ("phrases", "micro_phrases"):
        for phrase in package.get(collection_name, []):
            retune(phrase.get("chords", []))
            phrase.pop("vocal_variants", None)
            phrase.pop("vocal_key_variants", None)

    package["base_key_shift"] = new_shift
    package["vocal_style"] = plan
    package["key_style"] = {
        "label": "single_profile_key",
        "default_key_shift": new_shift,
        "available_key_shifts": [new_shift],
        "expression_strengths": [100],
        "manual_key_shift_range": [-3, 3],
        "selection": "single_best_user_range",
    }
    package_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return new_shift


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--experiment-name", required=True)
    parser.add_argument("--song-package", type=Path, required=True)
    parser.add_argument("--model")
    parser.add_argument("--index")
    parser.add_argument(
        "--user-voice",
        type=Path,
        help="Combined guided recording used to select the best key.",
    )
    parser.add_argument(
        "--full-variants",
        action="store_true",
        help="Legacy 5-strength x all-key-anchor rendering mode.",
    )
    parser.add_argument("--result-json", type=Path)
    args = parser.parse_args()

    package_path = args.song_package.resolve()
    if not package_path.is_file():
        raise FileNotFoundError(package_path)
    package_dir = package_path.parent
    package = json.loads(package_path.read_text(encoding="utf-8"))
    tag = safe_tag(args.experiment_name)
    model = args.model or f"{args.experiment_name}.pth"
    index = args.index or resolve_remote_index(
        args.host, args.experiment_name
    )

    base_shift = int(package.get("base_key_shift", 0))
    generated_root = package_dir / "generated_models" / tag
    if args.full_variants:
        base_manifest = copy_manifest(
            package_dir / "style_adaptation" / "style_variants.json",
            generated_root / "base",
        )
        key_style = package.get("key_style", {})
        anchor_shifts = sorted(
            {
                int(value)
                for value in key_style.get("available_key_shifts", [])
                if int(value) != base_shift
            }
        )
    else:
        if args.user_voice is None or not args.user_voice.is_file():
            raise FileNotFoundError(
                args.user_voice or "A valid --user-voice is required"
            )
        source_vocal = Path(package["audio"]["source_vocal"])
        if not source_vocal.is_file():
            raise FileNotFoundError(source_vocal)
        run(
            [
                sys.executable,
                str(SCRIPT_DIR / "prepare_rvc_guide.py"),
                "--user-voice",
                str(args.user_voice.resolve()),
                "--source-vocal",
                str(source_vocal.resolve()),
                "--output-dir",
                str(generated_root / "key_analysis"),
                "--direct-rvc",
                "--style-strengths",
                "100",
            ],
            "사용자 음역 분석 및 최적 키 선택",
        )
        analysis_manifest_path = (
            generated_root / "key_analysis" / "style_variants.json"
        )
        analysis_manifest = json.loads(
            analysis_manifest_path.read_text(encoding="utf-8")
        )
        selected_shift = int(analysis_manifest["base_key_shift"])
        selected_anchor = (
            package_dir
            / "key_anchors"
            / anchor_directory_name(selected_shift)
            / "style_variants.json"
        )
        base_manifest = copy_single_strength_manifest(
            selected_anchor,
            generated_root / "base",
            100,
        )
        anchor_shifts = []
    anchor_manifests: list[tuple[int, Path]] = []
    for shift in anchor_shifts:
        anchor_name = anchor_directory_name(shift)
        source = (
            package_dir
            / "key_anchors"
            / anchor_name
            / "style_variants.json"
        )
        anchor_manifests.append(
            (
                shift,
                copy_manifest(
                    source, generated_root / f"anchor_{anchor_name}"
                ),
            )
        )

    all_manifests = [base_manifest, *[item[1] for item in anchor_manifests]]
    for manifest in all_manifests:
        convert_manifest(
            manifest,
            args.host,
            model,
            index,
            f"{tag}_{manifest.parent.name}",
        )

    staging_path = package_dir / f".song_package.{tag}.next.json"
    shutil.copy2(package_path, staging_path)
    selected_shift = base_shift
    if not args.full_variants:
        selected_shift = configure_single_best_render(
            staging_path, base_manifest
        )
    run(
        [
            sys.executable,
            str(SCRIPT_DIR / "add_style_variants.py"),
            "--package",
            str(staging_path),
            "--manifest",
            str(base_manifest),
            "--directory-tag",
            tag,
        ],
        "기본 키 보컬 뱅크 생성",
    )
    for shift, manifest in anchor_manifests:
        run(
            [
                sys.executable,
                str(SCRIPT_DIR / "add_key_anchor.py"),
                "--package",
                str(staging_path),
                "--manifest",
                str(manifest),
            ],
            f"키 앵커 {shift:+d} 연결",
        )

    staged = json.loads(staging_path.read_text(encoding="utf-8"))
    rendered_base = json.loads(base_manifest.read_text(encoding="utf-8"))
    default_strength = int(rendered_base.get("default_strength", 25))
    default_variant = next(
        item
        for item in rendered_base["variants"]
        if int(item["strength"]) == default_strength
    )
    staged.setdefault("audio", {})["converted_vocal"] = str(
        Path(default_variant["converted_vocal"]).resolve()
    )
    staged["trained_voice_model"] = {
        "experiment": args.experiment_name,
        "model": model,
        "index": index,
        "render_root": str(generated_root.resolve()),
    }
    staging_path.write_text(
        json.dumps(staged, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    validation = validate_package(staging_path)
    backup_path = package_dir / f"song_package.before_{tag}.json"
    backup_number = 2
    while backup_path.exists():
        backup_path = package_dir / (
            f"song_package.before_{tag}_{backup_number}.json"
        )
        backup_number += 1
    shutil.copy2(package_path, backup_path)
    os.replace(staging_path, package_path)

    result = {
        "status": "complete",
        "experiment": args.experiment_name,
        "model": model,
        "index": index,
        "package": str(package_path),
        "backup": str(backup_path),
        "render_root": str(generated_root),
        "manifests_converted": len(all_manifests),
        "render_mode": (
            "full_variants" if args.full_variants else "single_best_key"
        ),
        "selected_key_shift": selected_shift,
        **validation,
    }
    result_path = (
        args.result_json.resolve()
        if args.result_json
        else generated_root / "refresh_result.json"
    )
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
