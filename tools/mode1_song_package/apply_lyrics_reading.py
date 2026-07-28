"""Merge user-supplied Korean pronunciation-reading lines into a song
package's phrases, one line per phrase in order.

This tool never contains or generates lyric text itself — it only takes
whatever local text file the user points it at (one reading line per
phrase; blank lines and lines starting with '#' or '[' are treated as
section markers and skipped) and copies each line into the matching
phrase's "lyrics_reading_ko" field, purely by position. The guided
recording song stage (see GuidedRecordingSession::setSongLines in the
C++ app) prefers this field over the original lyrics when present.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load_reading_lines(readings_path: Path) -> list[str]:
    lines: list[str] = []
    for raw_line in readings_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("["):
            continue
        lines.append(line)
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--readings", type=Path, required=True)
    parser.add_argument(
        "--target",
        choices=["phrases", "micro_phrases"],
        default="phrases",
        help="Which phrase collection to write lyrics_reading_ko onto.",
    )
    args = parser.parse_args()

    package_path = args.package.resolve()
    package = json.loads(package_path.read_text(encoding="utf-8"))
    phrases = package.get(args.target, [])
    if not phrases:
        raise SystemExit(f"Song package has no '{args.target}' entries.")

    reading_lines = load_reading_lines(args.readings.resolve())
    if len(reading_lines) == 0:
        raise SystemExit("Readings file contained no usable lines.")
    if len(reading_lines) != len(phrases):
        # Keep console output ASCII: Windows consoles default to cp949 here
        # and a stray em dash aborts the run before anything is written.
        print(
            "note: reading-line count differs from phrase count "
            f"({len(reading_lines)} lines vs {len(phrases)} '{args.target}' entries) "
            "- applying as many as available; this is fine, the guided "
            "recording song stage just cycles through whatever lines it gets.",
        )

    applied = 0
    for phrase, reading in zip(phrases, reading_lines):
        phrase["lyrics_reading_ko"] = reading
        applied += 1

    package_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "package": str(package_path),
        "target": args.target,
        "reading_lines_available": len(reading_lines),
        "phrases_total": len(phrases),
        "lines_applied": applied,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
