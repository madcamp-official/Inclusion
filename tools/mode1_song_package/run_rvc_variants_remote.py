"""Convert all prepared style variants on the configured GPU RVC server."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from pathlib import Path


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--model", default="rvc_user_long.pth")
    parser.add_argument(
        "--index",
        default="logs/rvc_user_long/added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Write converted WAVs here instead of next to the manifest.",
    )
    parser.add_argument(
        "--job-tag",
        default="",
        help="Unique tag for remote temporary filenames.",
    )
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir
        else manifest_path.parent
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    job_tag = re.sub(r"[^A-Za-z0-9_-]", "_", args.job_tag).strip("_")
    job_prefix = f"{job_tag}_" if job_tag else ""
    key_shift = int(manifest.get("base_key_shift", 0))
    key_tag = f"m{abs(key_shift):03d}" if key_shift < 0 else f"p{key_shift:03d}"
    converted = []
    total_variants = len(manifest["variants"])
    for position, variant in enumerate(manifest["variants"], start=1):
        strength = int(variant["strength"])
        rvc_pitch_shift = int(variant.get("rvc_pitch_shift", 0))
        print(
            f"[RVC {position}/{total_variants}] "
            f"key {key_shift:+d}, style {strength}",
            flush=True,
        )
        local_input = Path(variant["prepared_rvc_input"])
        remote_input = (
            f"/root/rvc-webui/input/{job_prefix}mode1_key_{key_tag}"
            f"_style_{strength:03d}.wav"
        )
        remote_output = (
            f"/root/rvc-webui/{job_prefix}output_mode1_key_{key_tag}"
            f"_style_{strength:03d}.wav"
        )
        local_output = output_dir / f"rvc_output_style_{strength:03d}.wav"
        run(["scp", str(local_input), f"{args.host}:{remote_input}"])
        command = (
            "cd /root/rvc-webui && .venv/bin/python rvc_infer.py "
            f"{shlex.quote(args.model)} {shlex.quote(remote_input)} "
            f"{shlex.quote(args.index)} {shlex.quote(remote_output)} "
            f"{rvc_pitch_shift}"
        )
        run(["ssh", "-o", "BatchMode=yes", args.host, command])
        run(["scp", f"{args.host}:{remote_output}", str(local_output)])
        variant["converted_vocal"] = str(local_output)
        converted.append({"strength": strength, "output": str(local_output)})

    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(converted, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
