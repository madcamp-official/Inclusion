"""Upload a prepared guide, run the existing GPU RVC CLI, and download it."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--host",
        required=True,
        help="SSH destination, for example username@vpn-host",
    )
    parser.add_argument("--model", default="rvc_user_long.pth")
    parser.add_argument(
        "--index",
        default="logs/rvc_user_long/added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index",
    )
    args = parser.parse_args()

    remote_input = "/root/rvc-webui/input/mode1_style_adapted.wav"
    remote_output = "/root/rvc-webui/output_mode1_style_adapted.wav"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run(["scp", str(args.input.resolve()), f"{args.host}:{remote_input}"])
    remote_command = (
        "cd /root/rvc-webui && .venv/bin/python rvc_infer.py "
        f"{args.model} {remote_input} {args.index} {remote_output} 0"
    )
    run(["ssh", "-o", "BatchMode=yes", args.host, remote_command])
    run(["scp", f"{args.host}:{remote_output}", str(args.output.resolve())])
    print(args.output.resolve())


if __name__ == "__main__":
    main()
