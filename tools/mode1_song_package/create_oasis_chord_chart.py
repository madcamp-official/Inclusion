"""Create a printable chord chart from the active Mode 1 Oasis package."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas


W, H = A4
INK = colors.HexColor("#17243B")
ACCENT = colors.HexColor("#B84A32")
MUTED = colors.HexColor("#667085")
GRID = colors.HexColor("#CBD5E1")
PALE = colors.HexColor("#F6F4F0")
CHANGE = colors.HexColor("#FFF0C7")
WHITE = colors.white


def fonts() -> None:
    pdfmetrics.registerFont(TTFont("UI", r"C:\Windows\Fonts\segoeui.ttf"))
    pdfmetrics.registerFont(TTFont("UIBold", r"C:\Windows\Fonts\segoeuib.ttf"))


def stamp(seconds: float) -> str:
    return f"{int(seconds // 60):02d}:{seconds % 60:05.2f}"


def load(package_path: Path) -> dict:
    package = json.loads(package_path.read_text(encoding="utf-8"))
    bpm = float(package["chord_bpm"])
    beat_seconds = 60.0 / bpm
    raw = [
        e for e in package["chord_timeline"]
        if str(e.get("chord", "N")) != "N"
    ]
    # The runtime package contains phrase-anchor duplicates. Retain changes and
    # repeated strikes separated by at least half a beat, then snap to the
    # package BPM grid for a musician-readable chart.
    cleaned = []
    for event in raw:
        t = float(event["start_sec"])
        chord = str(event["chord"])
        if cleaned and chord == cleaned[-1][1] and t - cleaned[-1][0] < beat_seconds * 0.5:
            continue
        cleaned.append((t, chord))

    events: dict[int, str] = {}
    errors = []
    for t, chord in cleaned:
        exact = t / beat_seconds
        beat = max(0, round(exact))
        errors.append(abs(exact - beat))
        # Prefer an actual harmonic change if two anchors land on one beat.
        if beat not in events or events[beat] != chord:
            events[beat] = chord

    ending = max(
        float(package["chord_timeline"][-1]["start_sec"]),
        max(float(p["source"]["end_sec"]) for p in package["phrases"]),
    )
    total_bars = math.ceil(ending / beat_seconds / 4)
    state = []
    current = ""
    for beat in range(total_bars * 4):
        if beat in events:
            current = events[beat]
        state.append(current or "N")

    lyrics_by_row: dict[int, list[dict]] = {}
    phrase_bars = []
    for phrase in package["phrases"]:
        start = float(phrase["source"]["start_sec"])
        exact = start / beat_seconds
        bar = max(1, int(exact // 4) + 1)
        phrase_bars.append(bar)
        lyrics_by_row.setdefault((bar - 1) // 4, []).append(
            {"bar": bar, "beat": exact, "time": start, "text": str(phrase["lyrics"])}
        )

    section_starts = {
        1: "Intro",
        phrase_bars[0]: "Verse 1",
        phrase_bars[6]: "Pre-chorus",
        phrase_bars[10]: "Chorus 1",
        phrase_bars[18]: "Verse 2",
        phrase_bars[24]: "Pre-chorus 2",
        phrase_bars[28]: "Chorus 2",
        phrase_bars[36]: "Solo / build",
        phrase_bars[40]: "Final chorus",
    }
    return {
        "bpm": bpm,
        "beat_seconds": beat_seconds,
        "events": events,
        "state": state,
        "lyrics": lyrics_by_row,
        "sections": section_starts,
        "bars": total_bars,
        "ending": ending,
        "event_count": len(events),
        "max_error": max(errors, default=0.0),
        "key_shift": int(package.get("base_key_shift", 0)),
    }


def footer(c: Canvas, page: int, chart: dict) -> None:
    c.setStrokeColor(GRID)
    c.line(28, 24, W - 28, 24)
    c.setFillColor(MUTED)
    c.setFont("UI", 7)
    c.drawString(28, 11, f"Mode 1 active package | {chart['bpm']:.1f} BPM | 4/4")
    c.drawRightString(W - 28, 11, str(page))


def cover(c: Canvas, chart: dict) -> None:
    c.setFillColor(INK)
    c.rect(0, H - 175, W, 175, fill=1, stroke=0)
    c.setFillColor(WHITE)
    c.setFont("UIBold", 26)
    c.drawString(38, H - 70, "DON'T LOOK BACK IN ANGER")
    c.setFont("UI", 14)
    c.drawString(39, H - 99, "Oasis - Mode 1 guitar chord chart")
    c.setFillColor(colors.HexColor("#F2B9A8"))
    c.setFont("UIBold", 9)
    c.drawString(39, H - 130, "ACTIVE PACKAGE TIMING • BEAT-BY-BEAT CHORDS • LYRIC CUES")

    cards = [
        ("PLAYBACK", [f"Tempo  {chart['bpm']:.1f} BPM", "Meter  4/4", f"Length  {stamp(chart['ending'])}"]),
        ("HOW TO READ", ["Each box is one bar.", "Yellow cells mark chord strikes or changes.", "A dash means hold the previous chord."]),
        ("KEY", [f"Runtime base shift  {chart['key_shift']:+d} semitones", "A -12 shift keeps the same chord names.", "Play the printed chord symbols."]),
    ]
    y = 545
    for title, lines in cards:
        c.setFillColor(PALE)
        c.setStrokeColor(GRID)
        c.roundRect(38, y, W - 76, 92, 8, fill=1, stroke=1)
        c.setFillColor(ACCENT)
        c.roundRect(38, y, 5, 92, 2, fill=1, stroke=0)
        c.setFillColor(INK)
        c.setFont("UIBold", 12)
        c.drawString(55, y + 66, title)
        c.setFillColor(MUTED)
        c.setFont("UI", 9)
        for i, line in enumerate(lines):
            c.drawString(55, y + 46 - i * 15, line)
        y -= 113

    c.setFillColor(INK)
    c.setFont("UIBold", 13)
    c.drawString(38, 185, "Suggested count")
    c.setFillColor(PALE)
    c.setStrokeColor(GRID)
    c.roundRect(38, 75, W - 76, 86, 8, fill=1, stroke=1)
    labels = ["1", "&", "2", "&", "3", "&", "4", "&"]
    pattern = ["D", "-", "D", "U", "-", "U", "D", "U"]
    for i, value in enumerate(labels):
        x = 125 + i * 48
        c.setFillColor(MUTED)
        c.setFont("UI", 8)
        c.drawCentredString(x, 133, value)
        c.setFillColor(INK)
        c.setFont("UIBold", 11)
        c.drawCentredString(x, 102, pattern[i])
    c.setFillColor(MUTED)
    c.setFont("UI", 8)
    c.drawString(52, 132, "COUNT")
    c.drawString(52, 101, "8THS")
    footer(c, 1, chart)


def bar(c: Canvas, chart: dict, number: int, x: float, top: float, width: float) -> None:
    height = 50
    bottom = top - height
    cell = width / 4
    first = (number - 1) * 4
    c.setFillColor(WHITE)
    c.setStrokeColor(GRID)
    c.roundRect(x, bottom, width, height, 4, fill=1, stroke=1)
    c.setFillColor(PALE)
    c.roundRect(x, top - 13, width, 13, 4, fill=1, stroke=0)
    c.rect(x, top - 13, width, 4, fill=1, stroke=0)
    section = chart["sections"].get(number)
    c.setFillColor(ACCENT if section else INK)
    c.setFont("UIBold", 5.8)
    suffix = f" | {section}" if section else ""
    c.drawString(x + 4, top - 9.5, f"M{number:03d} {stamp(first * chart['beat_seconds'])}{suffix}")
    for i in range(4):
        beat = first + i
        changed = beat in chart["events"]
        if changed:
            c.setFillColor(CHANGE)
            c.rect(x + i * cell + .5, bottom + .5, cell - 1, height - 14, fill=1, stroke=0)
        if i:
            c.setStrokeColor(GRID)
            c.line(x + i * cell, bottom, x + i * cell, top - 13)
        c.setFillColor(MUTED)
        c.setFont("UI", 5)
        c.drawCentredString(x + (i + .5) * cell, top - 20, str(i + 1))
        label = chart["state"][beat] if changed or i == 0 else "-"
        c.setFillColor(INK)
        c.setFont("UIBold", 8 if len(label) < 7 else 6.5)
        c.drawCentredString(x + (i + .5) * cell, top - 38, label)


def lyric_row(c: Canvas, chart: dict, first_bar: int, x: float, top: float, width: float) -> None:
    cues = chart["lyrics"].get((first_bar - 1) // 4, [])
    c.setFillColor(colors.HexColor("#F8FAFC"))
    c.roundRect(x, top - 42, width, 42, 4, fill=1, stroke=0)
    if not cues:
        c.setFillColor(MUTED)
        c.setFont("UI", 7)
        c.drawString(x + 5, top - 17, "Instrumental / vocal rest")
        return
    row_start = (first_bar - 1) * 4
    right = x + width - 4
    placements = []
    for cue in reversed(cues):
        anchor = x + max(3, min(width - 4, (cue["beat"] - row_start) / 16 * width))
        size = 7.2
        while size > 5 and pdfmetrics.stringWidth(cue["text"], "UIBold", size) > max(40, right - anchor):
            size -= .2
        text_width = pdfmetrics.stringWidth(cue["text"], "UIBold", size)
        px = max(x + 3, min(anchor, right - text_width))
        placements.append((cue, px, size))
        right = px - 5
    for cue, px, size in reversed(placements):
        c.setFillColor(ACCENT)
        c.setFont("UIBold", 5.2)
        c.drawString(px, top - 10, f"M{cue['bar']:03d} {stamp(cue['time'])}")
        c.setFillColor(INK)
        c.setFont("UIBold", size)
        c.drawString(px, top - 27, cue["text"])


def chart_page(c: Canvas, chart: dict, start: int, end: int, page: int) -> None:
    c.setFillColor(INK)
    c.setFont("UIBold", 16)
    c.drawString(28, H - 34, "DON'T LOOK BACK IN ANGER")
    c.setFillColor(MUTED)
    c.setFont("UI", 7.2)
    c.drawString(28, H - 49, f"M{start:03d}-M{end:03d} | Active Mode 1 chord timeline")
    margin, gap = 28, 4
    bw = (W - margin * 2 - gap * 3) / 4
    top = H - 68
    pitch = 105
    for offset, number in enumerate(range(start, end + 1)):
        row, col = divmod(offset, 4)
        bar(c, chart, number, margin + col * (bw + gap), top - row * pitch, bw)
    for row in range(math.ceil((end - start + 1) / 4)):
        lyric_row(c, chart, start + row * 4, margin, top - row * pitch - 55, W - margin * 2)
    footer(c, page, chart)


def create(package: Path, output: Path) -> None:
    fonts()
    chart = load(package)
    output.parent.mkdir(parents=True, exist_ok=True)
    c = Canvas(str(output), pagesize=A4)
    c.setTitle("Don't Look Back in Anger - Mode 1 chord chart")
    c.setAuthor("Mode 1 pipeline")
    cover(c, chart)
    c.showPage()
    page = 2
    for start in range(1, chart["bars"] + 1, 28):
        chart_page(c, chart, start, min(start + 27, chart["bars"]), page)
        c.showPage()
        page += 1
    c.save()
    print(json.dumps({
        "output": str(output), "pages": page - 1, "bars": chart["bars"],
        "printed_events": chart["event_count"], "bpm": chart["bpm"],
        "max_snap_error_beats": round(chart["max_error"], 4),
    }, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    create(args.package.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
