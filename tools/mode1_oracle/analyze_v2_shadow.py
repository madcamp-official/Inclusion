#!/usr/bin/env python3
"""Compare Active v2 Shadow reservations with Oracle B phrase targets."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values: list[float], q: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    lo = int(math.floor(position))
    hi = int(math.ceil(position))
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace_csv", type=Path)
    parser.add_argument("oracle_phrase_targets_csv", type=Path)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    trace = list(
        csv.DictReader(args.trace_csv.open(encoding="utf-8-sig"))
    )
    oracle_rows = list(
        csv.DictReader(
            args.oracle_phrase_targets_csv.open(encoding="utf-8-sig")
        )
    )
    oracle = {
        int(row["index"]): float(row["oracle_performance_sec"])
        for row in oracle_rows
        if int(row.get("inside_active_segment", "1"))
    }

    scheduled: dict[int, float] = {}
    cancelled: set[int] = set()
    for row in trace:
        phrase = int(row["index"])
        if row["type"] == "predictive_phrase_scheduled":
            scheduled[phrase] = float(row["time_sec"])
            cancelled.discard(phrase)
        elif row["type"] == "predictive_phrase_cancelled":
            cancelled.add(phrase)

    effective = {
        phrase: target
        for phrase, target in scheduled.items()
        if phrase not in cancelled
    }
    common = sorted(effective.keys() & oracle.keys())
    errors = [
        1000.0 * (effective[index] - oracle[index])
        for index in common
    ]
    absolute = [abs(value) for value in errors]
    states = [
        row for row in trace
        if row["type"] == "predictive_transport_state"
    ]
    first_oracle = min(oracle.values()) if oracle else math.nan
    locked_before_first = any(
        int(row["flags"]) == 2
        and float(row["time_sec"]) < first_oracle
        for row in states
    )
    half_beat_ms = 60_000.0 / 103.004 / 2.0
    result = {
        "trace": str(args.trace_csv),
        "oracle": str(args.oracle_phrase_targets_csv),
        "scheduled": len(scheduled),
        "cancelled": len(cancelled),
        "effective": len(effective),
        "oracle_active_phrases": len(oracle),
        "compared": len(common),
        "locked_before_first_vocal": locked_before_first,
        "timing_error_ms": {
            "median_signed": percentile(errors, 0.5),
            "median_absolute": percentile(absolute, 0.5),
            "p95_absolute": percentile(absolute, 0.95),
            "max_absolute": max(absolute) if absolute else math.nan,
            "over_100ms": sum(value > 100.0 for value in absolute),
            "half_beat_or_more": sum(
                value >= half_beat_ms for value in absolute
            ),
        },
        "per_phrase": [
            {
                "index": index,
                "oracle_sec": oracle[index],
                "shadow_sec": effective[index],
                "error_ms": 1000.0
                * (effective[index] - oracle[index]),
            }
            for index in common
        ],
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
