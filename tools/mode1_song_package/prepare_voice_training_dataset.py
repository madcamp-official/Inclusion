"""Combine a guided-recording profile's accepted voice clips into a single
WAV suitable for RVC training (see train_rvc_voice.py).

Reads every WAV under <profile-dir>/accepted (48 kHz mono, written by
VoiceRecorder::saveAsWav) in filename order — which already encodes stage
and take index (speaking_001.wav, vowel_004.wav, song_010.wav, ...) — and
concatenates them with a short silence gap between clips so the RVC
preprocessor's slicer doesn't blend two takes together at the splice point.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf


def find_latest_profile_dir(base_dir: Path) -> Path:
    candidates = sorted(
        (child for child in base_dir.iterdir() if child.is_dir()),
        key=lambda child: child.name,
    )
    if not candidates:
        raise SystemExit(f"No voice profile directories found under {base_dir}")
    return candidates[-1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile-dir",
        type=Path,
        help=(
            "A specific voice_profiles/<id> directory. If omitted, the most "
            "recently created one under --profiles-root is used."
        ),
    )
    parser.add_argument(
        "--profiles-root",
        type=Path,
        default=Path.home() / "Documents" / "VocalGuitarApp" / "voice_profiles",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gap-seconds", type=float, default=0.3)
    parser.add_argument(
        "--minimum-duration-seconds",
        type=float,
        default=0.0,
        help="Fail before training when accepted audio is shorter than this.",
    )
    args = parser.parse_args()

    profile_dir = args.profile_dir or find_latest_profile_dir(args.profiles_root)
    accepted_dir = profile_dir / "accepted"
    clip_paths = sorted(accepted_dir.glob("*.wav"))
    if not clip_paths:
        raise SystemExit(f"No accepted clips found under {accepted_dir}")

    sample_rate = None
    segments: list[np.ndarray] = []
    for clip_path in clip_paths:
        audio, sr = sf.read(clip_path, dtype="float32", always_2d=False)
        if audio.ndim > 1:
            audio = audio.mean(axis=1)
        if sample_rate is None:
            sample_rate = sr
        elif sr != sample_rate:
            raise SystemExit(
                f"{clip_path.name} is {sr} Hz, expected {sample_rate} Hz "
                "(all accepted clips should already be 48 kHz)."
            )
        segments.append(audio)
        if args.gap_seconds > 0:
            segments.append(np.zeros(int(sample_rate * args.gap_seconds), dtype="float32"))

    combined = np.concatenate(segments)
    duration_seconds = len(combined) / sample_rate
    if duration_seconds < args.minimum_duration_seconds:
        raise SystemExit(
            "Accepted clips are too short for retraining: "
            f"{duration_seconds:.1f}s / {args.minimum_duration_seconds:.1f}s required"
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(args.output, combined, sample_rate, subtype="PCM_24")

    manifest_path = profile_dir / "manifest.json"
    manifest_summary = {}
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_summary = {"profile_id": manifest.get("profile_id")}

    print(json.dumps({
        "profile_dir": str(profile_dir),
        **manifest_summary,
        "clips_combined": len(clip_paths),
        "duration_seconds": round(duration_seconds, 1),
        "output": str(args.output.resolve()),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
