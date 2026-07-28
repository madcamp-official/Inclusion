#!/usr/bin/env python3
"""Create stable timing metrics for a Mode1OfflineRenderer output directory."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("render_directory", type=Path)
    parser.add_argument("--renderer-output", type=Path)
    args = parser.parse_args()

    events = read_rows(args.render_directory / "phrase_events.csv")
    tracking = read_rows(args.render_directory / "score_tracking_events.csv")
    times = [float(row["time_sec"]) for row in events]
    gaps = [right - left for left, right in zip(times, times[1:])]
    thirds = max(1, len(gaps) // 3)
    late = gaps[-thirds:] if gaps else []

    metrics: dict[str, object] = {
        "phrase_events": len(events),
        "first_vocal_sec": times[0] if times else None,
        "last_vocal_sec": times[-1] if times else None,
        "sub_50ms_gaps": sum(gap < 0.0495 for gap in gaps),
        "minimum_gap_sec": min(gaps) if gaps else None,
        "median_late_third_gap_sec": (
            sorted(late)[len(late) // 2] if late else None
        ),
        "last_chord_event_index": (
            int(tracking[-1]["chord_event_index"]) if tracking else None
        ),
        "expired_phrases": (
            int(tracking[-1]["expired_phrases"]) if tracking else None
        ),
        "recovered_skipped_chords": (
            int(tracking[-1]["recovered_skipped_chords"]) if tracking else None
        ),
    }
    if args.renderer_output and args.renderer_output.exists():
        for line in args.renderer_output.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                if key in {"render_cpu_ms", "realtime_factor", "tempo_scale"}:
                    try:
                        metrics[key] = float(value)
                    except ValueError:
                        metrics[key] = value

    output = args.render_directory / "metrics.json"
    output.write_text(
        json.dumps(metrics, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metrics, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
