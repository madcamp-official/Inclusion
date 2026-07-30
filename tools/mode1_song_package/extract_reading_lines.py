"""Extract just the pronunciation-reading line out of a pasted lyrics block
and write it to a plain reading-lines file for apply_lyrics_reading.py.

This is a purely mechanical text splitter — it never contains or looks at
specific lyric content, it just groups the input by blank-line-separated
stanzas and, within each stanza, picks out the reading line by position.
Point it at a local text file you saved yourself (paste your own lyrics
block into it) in this per-stanza layout:

    [Section]
    <original-language line>
    <pronunciation-reading line>
    <translation line>

    <original-language line>
    <pronunciation-reading line>
    <translation line>
    ...

Blank lines separate stanzas; lines starting with '[' are section markers
and are skipped. Within each stanza, the reading line is selected by
--reading-line-index (0-based; default 1, i.e. the second of three lines).
Stanzas that don't have enough lines are skipped with a warning printed to
stderr (not to the output file), so you can fix them by hand afterwards.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def group_stanzas(raw_text: str) -> list[list[str]]:
    stanzas: list[list[str]] = []
    current: list[str] = []
    for raw_line in raw_text.splitlines():
        line = raw_line.strip()
        if not line:
            if current:
                stanzas.append(current)
                current = []
            continue
        if line.startswith("["):
            continue
        current.append(line)
    if current:
        stanzas.append(current)
    return stanzas


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lines-per-stanza", type=int, default=3)
    parser.add_argument("--reading-line-index", type=int, default=1)
    args = parser.parse_args()

    raw_text = args.input.read_text(encoding="utf-8")
    stanzas = group_stanzas(raw_text)

    reading_lines: list[str] = []
    skipped = 0
    for stanza_index, stanza in enumerate(stanzas):
        if len(stanza) != args.lines_per_stanza:
            print(
                f"warning: stanza {stanza_index} has {len(stanza)} line(s), "
                f"expected {args.lines_per_stanza} — skipped, fix by hand.",
                file=sys.stderr,
            )
            skipped += 1
            continue
        reading_lines.append(stanza[args.reading_line_index])

    args.output.write_text(
        "\n".join(reading_lines) + ("\n" if reading_lines else ""),
        encoding="utf-8",
    )
    print(
        f"Wrote {len(reading_lines)} reading line(s) to {args.output} "
        f"({skipped} stanza(s) skipped — see warnings above).",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
