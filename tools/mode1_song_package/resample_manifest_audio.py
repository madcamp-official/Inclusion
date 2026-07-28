"""Pre-resample converted vocals so the app can load key anchors quickly."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf


def linear_resample(audio: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
    if source_rate == target_rate:
        return audio
    output_length = round(len(audio) * target_rate / source_rate)
    positions = np.arange(output_length, dtype=np.float64) * (
        source_rate / target_rate
    )
    left = np.floor(positions).astype(np.int64)
    right = np.minimum(left + 1, len(audio) - 1)
    fraction = positions - left
    if audio.ndim == 2:
        fraction = fraction[:, np.newaxis]
    return audio[left] + fraction * (audio[right] - audio[left])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    outputs = []
    for variant in manifest["variants"]:
        source_path = Path(variant["converted_vocal"]).resolve()
        info = sf.info(source_path)
        if info.samplerate == args.sample_rate:
            outputs.append(str(source_path))
            continue
        audio, source_rate = sf.read(
            source_path, always_2d=False, dtype="float32"
        )
        converted = linear_resample(audio, source_rate, args.sample_rate)
        output_path = source_path.with_name(
            f"{source_path.stem}_{args.sample_rate // 1000}k.wav"
        )
        sf.write(
            output_path,
            converted,
            args.sample_rate,
            subtype=info.subtype,
        )
        variant["converted_vocal"] = str(output_path)
        outputs.append(str(output_path))

    manifest["playback_sample_rate"] = args.sample_rate
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(outputs, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
