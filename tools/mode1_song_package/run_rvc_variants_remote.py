"""Convert all prepared style variants on the configured GPU RVC server."""

from __future__ import annotations

import argparse
import json
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
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output_dir = manifest_path.parent
    key_shift = int(manifest.get("base_key_shift", 0))
    key_tag = f"m{abs(key_shift):03d}" if key_shift < 0 else f"p{key_shift:03d}"
    converted = []
    for variant in manifest["variants"]:
        strength = int(variant["strength"])
        local_input = Path(variant["prepared_rvc_input"])
        remote_input = (
            f"/root/rvc-webui/input/mode1_key_{key_tag}"
            f"_style_{strength:03d}.wav"
        )
        remote_output = (
            f"/root/rvc-webui/output_mode1_key_{key_tag}"
            f"_style_{strength:03d}.wav"
        )
        local_output = output_dir / f"rvc_output_style_{strength:03d}.wav"
        run(["scp", str(local_input), f"{args.host}:{remote_input}"])
        command = (
            "cd /root/rvc-webui && .venv/bin/python rvc_infer.py "
            f"{args.model} {remote_input} {args.index} {remote_output} 0"
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
