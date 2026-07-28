"""Create a portrait, beat-accurate Bansanka guitar chord chart PDF."""

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


PAGE_SIZE = A4
PAGE_WIDTH, PAGE_HEIGHT = PAGE_SIZE

NAVY = colors.HexColor("#17243B")
BLUE = colors.HexColor("#246B78")
MUTED = colors.HexColor("#667085")
GRID = colors.HexColor("#CBD5E1")
PALE = colors.HexColor("#F4F7FA")
CHANGE = colors.HexColor("#FFF4CC")
MODULATION = colors.HexColor("#FDE8E7")
WHITE = colors.white


SECTION_LABELS = {
    1: "Intro",
    5: "Block A",
    14: "Block B",
    20: "Block C",
    33: "Block A2",
    42: "Block D",
    53: "Block E",
    64: "Block F",
    72: "Modulation pickup",
    73: "Final key",
    89: "Outro",
}


def register_fonts() -> None:
    pdfmetrics.registerFont(TTFont("Malgun", r"C:\Windows\Fonts\malgun.ttf"))
    pdfmetrics.registerFont(
        TTFont("MalgunBold", r"C:\Windows\Fonts\malgunbd.ttf")
    )


def time_label(seconds: float) -> str:
    minutes = int(max(0.0, seconds) // 60)
    remainder = max(0.0, seconds) - minutes * 60
    return f"{minutes:02d}:{remainder:05.2f}"


def load_chart(package_path: Path) -> dict:
    package = json.loads(package_path.read_text(encoding="utf-8"))
    bpm = float(package["chord_bpm"])
    beat_seconds = 60.0 / bpm
    timeline = [
        event
        for event in package["chord_timeline"]
        if event["chord"] != "N"
    ]
    phase = float(timeline[0]["start_sec"])

    events: dict[int, tuple[str, str]] = {}
    maximum_error = 0.0
    for event in timeline:
        exact_beat = (float(event["start_sec"]) - phase) / beat_seconds
        beat_index = round(exact_beat)
        maximum_error = max(maximum_error, abs(exact_beat - beat_index))
        events[beat_index] = (
            event["chord"],
            event.get("original_chord", event["chord"]),
        )

    ending = next(
        (
            float(event["start_sec"])
            for event in package["chord_timeline"]
            if event["chord"] == "N" and float(event["start_sec"]) > phase
        ),
        max(float(event["start_sec"]) for event in timeline) + beat_seconds,
    )
    total_beats = round((ending - phase) / beat_seconds)
    total_bars = math.ceil(total_beats / 4)

    current_states: list[str] = []
    original_states: list[str] = []
    current = ""
    original = ""
    for beat in range(total_bars * 4):
        if beat in events:
            current, original = events[beat]
        current_states.append(current)
        original_states.append(original)

    lyrics_by_row: dict[int, list[dict]] = {}
    for phrase in package.get("phrases", []):
        source_start = float(phrase["source"]["start_sec"])
        exact_beat = max(0.0, (source_start - phase) / beat_seconds)
        beat_index = round(exact_beat)
        bar_number = max(1, beat_index // 4 + 1)
        kana = "".join(
            str(note.get("kana", ""))
            .replace("'", "")
            .replace("-", "")
            .replace(" ", "")
            .replace("\u3000", "")
            for note in phrase.get("notes", [])
        )
        cue = {
            "bar": bar_number,
            "exact_beat": exact_beat,
            "start_sec": source_start,
            "lyrics": str(phrase.get("lyrics", "")).replace("\u3000", " "),
            "kana": kana,
        }
        lyrics_by_row.setdefault((bar_number - 1) // 4, []).append(cue)

    for cues in lyrics_by_row.values():
        cues.sort(key=lambda cue: cue["exact_beat"])

    return {
        "bpm": bpm,
        "beat_seconds": beat_seconds,
        "phase": phase,
        "ending": ending,
        "events": events,
        "total_bars": total_bars,
        "current_states": current_states,
        "original_states": original_states,
        "lyrics_by_row": lyrics_by_row,
        "maximum_error": maximum_error,
    }


def footer(canvas: Canvas, page_number: int) -> None:
    canvas.setStrokeColor(GRID)
    canvas.line(28, 24, PAGE_WIDTH - 28, 24)
    canvas.setFillColor(MUTED)
    canvas.setFont("Malgun", 7.2)
    canvas.drawString(
        28,
        11,
        "Bansanka Mode 1 guitar chart | BPM 103.004 | 4/4",
    )
    canvas.drawRightString(PAGE_WIDTH - 28, 11, f"{page_number}")


def rounded_card(
    canvas: Canvas,
    x: float,
    y: float,
    width: float,
    height: float,
    title: str,
    lines: list[str],
    accent=BLUE,
) -> None:
    canvas.setFillColor(PALE)
    canvas.setStrokeColor(GRID)
    canvas.roundRect(x, y, width, height, 8, fill=1, stroke=1)
    canvas.setFillColor(accent)
    canvas.roundRect(x, y, 5, height, 2, fill=1, stroke=0)
    canvas.setFillColor(NAVY)
    canvas.setFont("MalgunBold", 12)
    canvas.drawString(x + 16, y + height - 22, title)
    canvas.setFillColor(MUTED)
    canvas.setFont("Malgun", 8.3)
    line_y = y + height - 40
    for line in lines:
        canvas.drawString(x + 16, line_y, line)
        line_y -= 14


def draw_cover(canvas: Canvas, chart: dict) -> None:
    canvas.setFillColor(NAVY)
    canvas.rect(0, PAGE_HEIGHT - 150, PAGE_WIDTH, 150, fill=1, stroke=0)
    canvas.setFillColor(WHITE)
    canvas.setFont("MalgunBold", 25)
    canvas.drawString(38, PAGE_HEIGHT - 68, "만찬가 기타 연주용 코드 차트")
    canvas.setFont("Malgun", 10.5)
    canvas.drawString(
        39,
        PAGE_HEIGHT - 96,
        "Mode 1 오디오 정렬 데이터 기반 | 92마디 | 코드 + 가사 + 히라가나",
    )
    canvas.setFillColor(colors.HexColor("#A7D8DE"))
    canvas.setFont("MalgunBold", 8.5)
    canvas.drawString(
        39,
        PAGE_HEIGHT - 122,
        "세로형 연주 시트 · 한 줄 4마디 · 가사는 해당 시작 박자 아래에 표시",
    )

    rounded_card(
        canvas,
        38,
        566,
        PAGE_WIDTH - 76,
        96,
        "재생 설정",
        [
            f"템포: {chart['bpm']:.3f} BPM",
            "박자: 4/4 | 한 칸 = 1박",
            f"첫 코드: {time_label(chart['phase'])}",
            f"종료: {time_label(chart['ending'])}",
        ],
    )
    rounded_card(
        canvas,
        38,
        448,
        PAGE_WIDTH - 76,
        96,
        "내 기본키로 연주",
        [
            "키 슬라이더: 0",
            "큰 검정 코드만 연주",
            "첫 루프: Eb/G - Ab - Bb - Cm7",
            "바레가 어렵다면 같은 루트의 간단한 코드 폼을 사용",
        ],
        accent=NAVY,
    )
    rounded_card(
        canvas,
        38,
        330,
        PAGE_WIDTH - 76,
        96,
        "원곡 코드로 연주",
        [
            "키 슬라이더: +5 또는 +17",
            "작은 청록 코드만 연주",
            "첫 루프: Ab/C - Db - Eb - Fm7",
            "+17에서는 원곡 옥타브를 그대로 사용",
        ],
        accent=BLUE,
    )

    canvas.setFillColor(NAVY)
    canvas.setFont("MalgunBold", 14)
    canvas.drawString(38, 296, "차트 읽는 법")
    instructions = [
        "각 마디의 1, 2, 3, 4 칸은 한 박입니다. '-'는 이전 코드를 유지합니다.",
        "노란 칸은 실제 코드 변경 박자입니다. 큰 검정은 기본키, 작은 청록은 원곡키입니다.",
        "가사는 해당 구절이 시작되는 박자 아래에 배치했습니다. M 번호와 시간도 함께 표시됩니다.",
        "붉은 72-73마디는 마지막 반음 전조 구간입니다.",
    ]
    y = 273
    for index, instruction in enumerate(instructions, 1):
        canvas.setFillColor(BLUE)
        canvas.setFont("MalgunBold", 8.5)
        canvas.drawString(44, y, f"{index}.")
        canvas.setFillColor(MUTED)
        canvas.setFont("Malgun", 8.5)
        canvas.drawString(62, y, instruction)
        y -= 27

    canvas.setFillColor(NAVY)
    canvas.setFont("MalgunBold", 14)
    canvas.drawString(38, 150, "스트로크 연습")
    canvas.setStrokeColor(GRID)
    canvas.setFillColor(PALE)
    canvas.roundRect(38, 54, PAGE_WIDTH - 76, 78, 8, fill=1, stroke=1)
    labels = [
        ("카운트", "1", "&", "2", "&", "3", "&", "4", "&"),
        ("다운 패턴", "D", "-", "D", "-", "D", "-", "D", "-"),
        ("8비트 패턴", "D", "-", "D", "U", "-", "U", "D", "U"),
    ]
    label_width = 88
    cell_width = (PAGE_WIDTH - 76 - label_width - 20) / 8
    for row, values in enumerate(labels):
        row_y = 109 - row * 22
        canvas.setFont("MalgunBold" if row == 0 else "Malgun", 8.5)
        canvas.setFillColor(NAVY if row == 0 else MUTED)
        canvas.drawString(50, row_y, values[0])
        for column, value in enumerate(values[1:]):
            canvas.drawCentredString(
                38 + label_width + cell_width * (column + 0.5),
                row_y,
                value,
            )
    footer(canvas, 1)


def draw_bar(
    canvas: Canvas,
    chart: dict,
    bar_number: int,
    x: float,
    top_y: float,
    width: float,
) -> None:
    height = 52
    bottom = top_y - height
    beat_width = width / 4
    first_beat = (bar_number - 1) * 4
    modulation = bar_number in (72, 73)

    canvas.setFillColor(MODULATION if modulation else WHITE)
    canvas.setStrokeColor(colors.HexColor("#D0443E") if modulation else GRID)
    canvas.roundRect(x, bottom, width, height, 4, fill=1, stroke=1)

    canvas.setFillColor(MODULATION if modulation else PALE)
    canvas.roundRect(x, top_y - 13, width, 13, 4, fill=1, stroke=0)
    canvas.rect(x, top_y - 13, width, 5, fill=1, stroke=0)
    canvas.setFillColor(NAVY)
    canvas.setFont("MalgunBold", 6.1)
    section = SECTION_LABELS.get(bar_number, "")
    section_text = f" | {section}" if section else ""
    canvas.drawString(
        x + 4,
        top_y - 9.5,
        f"M{bar_number:02d} {time_label(chart['phase'] + first_beat * chart['beat_seconds'])}{section_text}",
    )

    for beat in range(4):
        cell_x = x + beat * beat_width
        beat_index = first_beat + beat
        changed = beat_index in chart["events"]
        if changed:
            canvas.setFillColor(CHANGE)
            canvas.rect(
                cell_x + 0.5,
                bottom + 0.5,
                beat_width - 1,
                height - 14,
                fill=1,
                stroke=0,
            )
        if beat > 0:
            canvas.setStrokeColor(GRID)
            canvas.line(cell_x, bottom, cell_x, top_y - 13)

        canvas.setFillColor(MUTED)
        canvas.setFont("Malgun", 5.3)
        canvas.drawCentredString(
            cell_x + beat_width / 2, top_y - 20, str(beat + 1)
        )

        force_label = beat == 0
        current = (
            chart["current_states"][beat_index]
            if changed or force_label
            else "-"
        )
        original = (
            chart["original_states"][beat_index]
            if changed or force_label
            else "-"
        )
        canvas.setFillColor(NAVY)
        canvas.setFont("MalgunBold", 8.0 if len(current) <= 6 else 6.6)
        canvas.drawCentredString(
            cell_x + beat_width / 2, top_y - 34, current
        )
        canvas.setFillColor(BLUE)
        canvas.setFont("Malgun", 5.9 if len(original) <= 7 else 5.0)
        canvas.drawCentredString(
            cell_x + beat_width / 2, top_y - 46, original
        )


def fitted_font_size(
    text: str,
    font_name: str,
    maximum: float,
    minimum: float,
    width: float,
) -> float:
    size = maximum
    while size > minimum and pdfmetrics.stringWidth(
        text, font_name, size
    ) > width:
        size -= 0.25
    return max(minimum, size)


def draw_lyrics_under_score(
    canvas: Canvas,
    chart: dict,
    first_bar: int,
    x: float,
    top_y: float,
    width: float,
) -> None:
    height = 47
    bottom = top_y - height
    row_index = (first_bar - 1) // 4
    cues = chart["lyrics_by_row"].get(row_index, [])
    canvas.setFillColor(colors.HexColor("#F8FAFC"))
    canvas.roundRect(x, bottom, width, height, 4, fill=1, stroke=0)
    canvas.setStrokeColor(colors.HexColor("#E3EAF1"))
    canvas.line(x, top_y, x + width, top_y)

    if not cues:
        canvas.setFillColor(MUTED)
        canvas.setFont("Malgun", 7)
        canvas.drawString(x + 6, top_y - 16, "Instrumental / vocal rest")
        return

    row_start_beat = (first_bar - 1) * 4
    positions = [
        min(
            width - 3,
            max(3, (cue["exact_beat"] - row_start_beat) / 16 * width),
        )
        for cue in cues
    ]
    placements: list[tuple[float, float, float, str, str]] = [
        (0, 0, 0, "", "") for _ in cues
    ]
    right_limit = x + width - 3
    for index in range(len(cues) - 1, -1, -1):
        cue = cues[index]
        anchor_x = x + positions[index]
        available = max(38, right_limit - anchor_x - 5)
        prefix = f"M{cue['bar']:02d} {time_label(cue['start_sec'])}"
        lyric_size = fitted_font_size(
            cue["lyrics"], "MalgunBold", 8.4, 5.3, available
        )
        kana_text = cue["kana"] or "-"
        kana_size = fitted_font_size(
            kana_text, "Malgun", 6.1, 4.6, available
        )
        required_width = max(
            pdfmetrics.stringWidth(prefix, "MalgunBold", 5.3),
            pdfmetrics.stringWidth(cue["lyrics"], "MalgunBold", lyric_size),
            pdfmetrics.stringWidth(kana_text, "Malgun", kana_size),
        )
        cue_x = min(anchor_x, right_limit - required_width)
        cue_x = max(x + 3, cue_x)
        placements[index] = (
            cue_x,
            lyric_size,
            kana_size,
            prefix,
            kana_text,
        )
        right_limit = cue_x - 5

    for cue, placement in zip(cues, placements):
        cue_x, lyric_size, kana_size, prefix, kana_text = placement
        canvas.setFillColor(BLUE)
        canvas.setFont("MalgunBold", 5.3)
        canvas.drawString(cue_x, top_y - 9, prefix)
        canvas.setFillColor(NAVY)
        canvas.setFont("MalgunBold", lyric_size)
        canvas.drawString(cue_x, top_y - 23, cue["lyrics"])

        canvas.setFillColor(MUTED)
        canvas.setFont("Malgun", kana_size)
        canvas.drawString(cue_x, top_y - 36, kana_text)


def draw_chart_page(
    canvas: Canvas,
    chart: dict,
    start_bar: int,
    end_bar: int,
    page_number: int,
) -> None:
    canvas.setFillColor(NAVY)
    canvas.setFont("MalgunBold", 17)
    canvas.drawString(28, PAGE_HEIGHT - 34, "만찬가 코드 차트")
    canvas.setFillColor(MUTED)
    canvas.setFont("Malgun", 7.5)
    canvas.drawString(
        28,
        PAGE_HEIGHT - 50,
        f"M{start_bar:02d}-M{end_bar:02d} | 큰 검정: 기본키(-17, slider 0) | 작은 청록: 원곡키",
    )
    canvas.setFillColor(CHANGE)
    canvas.rect(PAGE_WIDTH - 128, PAGE_HEIGHT - 49, 9, 9, fill=1, stroke=0)
    canvas.setFillColor(MUTED)
    canvas.drawString(PAGE_WIDTH - 115, PAGE_HEIGHT - 49, "코드 변경 박자")

    margin_x = 28
    gap_x = 4
    bar_width = (PAGE_WIDTH - 2 * margin_x - 3 * gap_x) / 4
    first_top = PAGE_HEIGHT - 69
    row_pitch = 119
    bars = list(range(start_bar, end_bar + 1))

    for offset, bar_number in enumerate(bars):
        row = offset // 4
        column = offset % 4
        draw_bar(
            canvas,
            chart,
            bar_number,
            margin_x + column * (bar_width + gap_x),
            first_top - row * row_pitch,
            bar_width,
        )

    row_count = math.ceil(len(bars) / 4)
    for row in range(row_count):
        draw_lyrics_under_score(
            canvas,
            chart,
            start_bar + row * 4,
            margin_x,
            first_top - row * row_pitch - 57,
            PAGE_WIDTH - 2 * margin_x,
        )
    footer(canvas, page_number)


def create_pdf(package_path: Path, output_path: Path) -> None:
    register_fonts()
    chart = load_chart(package_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    canvas = Canvas(str(output_path), pagesize=PAGE_SIZE)
    canvas.setTitle("Bansanka portrait guitar chord chart")
    canvas.setAuthor("Mode 1 pipeline")

    draw_cover(canvas, chart)
    canvas.showPage()

    bars_per_page = 24
    page_number = 2
    for start_bar in range(1, chart["total_bars"] + 1, bars_per_page):
        end_bar = min(start_bar + bars_per_page - 1, chart["total_bars"])
        draw_chart_page(canvas, chart, start_bar, end_bar, page_number)
        canvas.showPage()
        page_number += 1
    canvas.save()
    print(
        json.dumps(
            {
                "output": str(output_path),
                "pages": page_number - 1,
                "bars": chart["total_bars"],
                "bpm": chart["bpm"],
                "first_chord_sec": chart["phase"],
                "ending_sec": chart["ending"],
                "maximum_quantization_error_beats": chart["maximum_error"],
            },
            ensure_ascii=False,
            indent=2,
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    create_pdf(args.package.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
