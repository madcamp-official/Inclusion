#!/usr/bin/env python3
"""Summarize Mode 1's lock-free audio-thread trace.

Offline numbers describe callback/sample scheduling only. They do not include
physical interface round-trip latency and must not be reported as live latency.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict, deque
from pathlib import Path
from statistics import median


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + fraction * (ordered[upper] - ordered[lower])


def summarize(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "first_ms": values[0] if values else None,
        "median_ms": median(values) if values else None,
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "max_ms": max(values) if values else None,
    }


def load_events(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def analyze(events: list[dict[str, str]], sample_rate: float) -> dict:
    by_type: dict[str, list[dict[str, str]]] = defaultdict(list)
    for event in events:
        by_type[event["type"]].append(event)

    detector_completion_ms = [
        (int(event["related_sample"]) - int(event["sample"]))
        * 1000.0
        / sample_rate
        for event in by_type["onset_detected"]
    ]

    pending_requests: dict[int, deque[int]] = defaultdict(deque)
    request_to_output_ms: list[float] = []
    unmatched_outputs = 0
    for event in events:
        event_type = event["type"]
        phrase_index = int(event["index"])
        sample = int(event["sample"])
        if event_type == "phrase_requested":
            pending_requests[phrase_index].append(sample)
        elif event_type == "vocal_first_output":
            queue = pending_requests[phrase_index]
            if not queue:
                unmatched_outputs += 1
                continue
            request_sample = queue.popleft()
            request_to_output_ms.append(
                (sample - request_sample) * 1000.0 / sample_rate
            )

    unmatched_requests = sum(len(queue) for queue in pending_requests.values())
    first_onset_sample = (
        int(by_type["onset_detected"][0]["sample"])
        if by_type["onset_detected"]
        else None
    )
    first_request_sample = (
        int(by_type["phrase_requested"][0]["sample"])
        if by_type["phrase_requested"]
        else None
    )
    first_onset_to_first_request_ms = (
        (first_request_sample - first_onset_sample) * 1000.0 / sample_rate
        if first_onset_sample is not None and first_request_sample is not None
        else None
    )

    return {
        "schema_version": 1,
        "sample_rate": sample_rate,
        "warning": (
            "Offline trace metrics exclude physical input/output latency and "
            "must not be presented as live round-trip latency."
        ),
        "event_counts": {
            event_type: len(rows) for event_type, rows in sorted(by_type.items())
        },
        "onset_detector_completion": summarize(detector_completion_ms),
        "phrase_request_to_first_output": summarize(request_to_output_ms),
        "first_onset_to_first_phrase_request_ms": (
            first_onset_to_first_request_ms
        ),
        "unmatched_phrase_requests": unmatched_requests,
        "unmatched_first_outputs": unmatched_outputs,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace_csv", type=Path)
    parser.add_argument("--sample-rate", type=float, default=48_000.0)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    result = analyze(load_events(args.trace_csv), args.sample_rate)
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output_json is not None:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
