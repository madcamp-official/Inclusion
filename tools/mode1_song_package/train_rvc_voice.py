"""Retrain the user's RVC voice model on the GPU server from a combined
voice-profile WAV (see prepare_voice_training_dataset.py).

Mirrors exactly what the RVC WebUI's Preprocess / Extract / Train / Train
Index buttons do — the underlying commands were recovered read-only from
webui.py and configs/config.py on the documented server
(docs/GPU_VOICE_CONVERSION_HANDOFF_2026-07-28.md), since no headless CLI
recipe had been preserved before. Runs the same 4 stages the doc describes
running through the WebUI by hand:

    preprocess -> extract F0 (RMVPE/GPU) -> extract HuBERT feature -> train
    -> build the retrieval index

Requires SSH key auth already set up for --host (same precondition as the
existing run_rvc_remote.py / run_rvc_variants_remote.py scripts). Never put
passwords or key contents in code or command-line arguments.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

REMOTE_ROOT = "/root/rvc-webui"
PYTHON_CMD = f"{REMOTE_ROOT}/.venv/bin/python"


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def ssh(host: str, remote_command: str) -> None:
    run(["ssh", "-o", "BatchMode=yes", host, remote_command])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="e.g. root@172.10.5.154")
    parser.add_argument("--voice-wav", type=Path, required=True)
    parser.add_argument(
        "--experiment-name",
        required=True,
        help="New RVC experiment/model name, e.g. rvc_user_2026_08_01",
    )
    parser.add_argument("--sample-rate", choices=["32k", "40k", "48k"], default="40k")
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--save-every", type=int, default=25)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--num-processes", type=int, default=8)
    parser.add_argument("--gpu-index", default="0")
    parser.add_argument("--version", default="v2")
    parser.add_argument("--skip-preprocess", action="store_true")
    parser.add_argument("--skip-extract", action="store_true")
    parser.add_argument("--skip-train", action="store_true")
    parser.add_argument("--skip-index", action="store_true")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the remote commands instead of running them.",
    )
    args = parser.parse_args()

    exp = args.experiment_name
    dataset_dir = f"datasets/{exp}"
    log_dir = f"logs/{exp}"
    remote_wav = f"{REMOTE_ROOT}/{dataset_dir}/user_voice.wav"

    sample_rate_hz = {"32k": 32000, "40k": 40000, "48k": 48000}[args.sample_rate]
    steps: list[tuple[str, str]] = []

    steps.append((
        "preprocess",
        f'"{PYTHON_CMD}" train/preprocess.py "{dataset_dir}" {sample_rate_hz} '
        f'{args.num_processes} "{REMOTE_ROOT}/{log_dir}" False 3.7',
    ))
    steps.append((
        "extract_f0",
        f'"{PYTHON_CMD}" train/dataset/extract_f0.py cuda 1 0 {args.gpu_index} '
        f'"{REMOTE_ROOT}/{log_dir}" True',
    ))
    steps.append((
        "extract_feature",
        f'"{PYTHON_CMD}" train/dataset/extract_hubert_feature.py cuda:{args.gpu_index} '
        f'1 0 {args.gpu_index} "{REMOTE_ROOT}/{log_dir}" {args.version} True',
    ))
    steps.append((
        "train",
        f'"{PYTHON_CMD}" train/train.py -e "{exp}" -sr {args.sample_rate} -f0 1 '
        f'-bs {args.batch_size} -g {args.gpu_index} -te {args.epochs} '
        f"-se {args.save_every} -pg assets/pretrained_v2/f0G40k.pth "
        f"-pd assets/pretrained_v2/f0D40k.pth -l 1 -c 1 -sw 1 -v {args.version}",
    ))
    steps.append((
        "train_index",
        f'"{PYTHON_CMD}" train/train_index.py "{exp}" {args.version} '
        f'"assets/indices" 40',
    ))

    skip_flags = {
        "preprocess": args.skip_preprocess,
        "extract_f0": args.skip_extract,
        "extract_feature": args.skip_extract,
        "train": args.skip_train,
        "train_index": args.skip_index,
    }

    if args.dry_run:
        print(f"mkdir -p {REMOTE_ROOT}/{dataset_dir}")
        print(f"scp {args.voice_wav} -> {args.host}:{remote_wav}")
        for name, command in steps:
            marker = " (skipped)" if skip_flags[name] else ""
            print(f"[{name}]{marker} cd {REMOTE_ROOT} && {command}")
        return

    if not skip_flags["preprocess"]:
        ssh(args.host, f"mkdir -p {REMOTE_ROOT}/{dataset_dir}")
        run(["scp", str(args.voice_wav.resolve()), f"{args.host}:{remote_wav}"])

    for name, command in steps:
        if skip_flags[name]:
            print(f"[{name}] skipped")
            continue
        print(f"[{name}] running...")
        ssh(args.host, f"cd {REMOTE_ROOT} && {command}")
        print(f"[{name}] done")

    weights_path = f"{REMOTE_ROOT}/assets/weights/{exp}.pth"
    ssh(
        args.host,
        f"echo '--- resulting artifacts ---' && "
        f"ls -la {weights_path} 2>&1 && "
        f"ls -la {REMOTE_ROOT}/{log_dir}/added_*.index 2>&1",
    )
    print(
        f"Done. Use --model {exp}.pth and the printed index path with "
        "run_rvc_remote.py / run_rvc_variants_remote.py."
    )


if __name__ == "__main__":
    main()
