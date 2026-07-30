#!/usr/bin/env python3
"""Measure conservative audible-onset anchors for Mode 1 vocal clips.

The automatic detector intentionally does not label an energy edge as a
phonetic vowel onset.  A reviewed ``vowel_onset_sec`` can be added later and
will take precedence in the real-time scheduler.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
import soundfile as sf


def measure_audible_onset(
    path: Path, content_offset: float, threshold_db: float
) -> tuple[float, float]:
    audio, sample_rate = sf.read(path, always_2d=True, dtype="float32")
    mono = np.max(np.abs(audio), axis=1)
    start = max(0, min(len(mono), round(content_offset * sample_rate)))
    window = max(1, round(0.010 * sample_rate))
    hop = max(1, round(0.0025 * sample_rate))
    if len(mono) - start < window:
        return 0.0, 0.0

    # Use both an absolute floor and a clip-relative floor.  Requiring two
    # consecutive windows rejects isolated clicks without adding >5 ms delay.
    peak = float(np.max(mono[start:]))
    threshold = max(10.0 ** (threshold_db / 20.0), peak * 0.035)
    hit = None
    for offset in range(start, len(mono) - window + 1, hop):
        rms = math.sqrt(float(np.mean(np.square(mono[offset : offset + window]))))
        if rms < threshold:
            continue
        next_offset = offset + hop
        if next_offset + window <= len(mono):
            next_rms = math.sqrt(
                float(np.mean(np.square(mono[next_offset : next_offset + window])))
            )
            if next_rms < threshold:
                continue
        hit = offset
        break

    if hit is None:
        return 0.0, 0.0
    relative = max(0.0, (hit - start) / sample_rate)
    # A quarter-second gap is more likely a deliberately silent/fragmented
    # clip than a consonant lead.  Do not let an automatic detector pull such
    # a phrase far ahead; flag it as untrusted instead.
    if relative > 0.250:
        return 0.0, 0.0
    confidence = min(1.0, peak / max(threshold, 1.0e-9) / 8.0)
    return relative, confidence


def annotate(package_path: Path, output_path: Path, threshold_db: float) -> dict:
    package = json.loads(package_path.read_text(encoding="utf-8"))
    phrases = package.get("micro_phrases") or package.get("phrases") or []
    measured: list[float] = []
    missing: list[str] = []
    for phrase in phrases:
        vocal = phrase.get("vocal", {})
        directory = vocal.get("directory", "vocals")
        path = package_path.parent / directory / str(vocal.get("file", ""))
        if not path.is_file():
            missing.append(str(path))
            continue
        content_offset = float(vocal.get("content_offset_sec", 0.0))
        onset, confidence = measure_audible_onset(
            path, content_offset, threshold_db
        )
        vocal["sync"] = {
            "anchor_type": "audible_onset_auto",
            "audible_onset_sec": round(onset, 6),
            "confidence": round(confidence, 4),
            "detector_version": 1,
        }
        measured.append(onset)

    if missing:
        raise FileNotFoundError(
            "Missing vocal clips:\n" + "\n".join(missing[:10])
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    ordered = sorted(measured)
    return {
        "package": str(package_path),
        "output": str(output_path),
        "phrases": len(measured),
        "nonzero_anchors": sum(value > 0.0 for value in measured),
        "median_ms": round(1000.0 * ordered[len(ordered) // 2], 3)
        if ordered
        else 0.0,
        "p95_ms": round(
            1000.0 * ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))],
            3,
        )
        if ordered
        else 0.0,
        "max_ms": round(1000.0 * max(ordered), 3) if ordered else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--threshold-db", type=float, default=-48.0)
    args = parser.parse_args()
    output = args.output or args.package
    print(
        json.dumps(
            annotate(args.package, output, args.threshold_db),
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
