#!/usr/bin/env python3
"""Convert a monophonic vocal MusicXML part into Mode 1 score inputs.

The converter intentionally does not require lyrics in MusicXML.  It assigns
the non-empty lines of a separate lyrics file to melodic notes, using rests
and the relative syllable counts as phrase-boundary hints.  It also renders a
simple sine guide used only for score-to-source DTW alignment.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf


STEP_TO_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


@dataclass
class RawNote:
    start_quarter: float
    end_quarter: float
    midi: int
    measure: int


def midi_value(pitch: ET.Element) -> int:
    step = STEP_TO_PC[pitch.findtext("step", "C")]
    alter = int(pitch.findtext("alter", "0"))
    octave = int(pitch.findtext("octave", "4"))
    return 12 * (octave + 1) + step + alter


def parse_musicxml(path: Path) -> tuple[list[RawNote], list[tuple[float, float]], float]:
    root = ET.parse(path).getroot()
    part = root.find("./part")
    if part is None:
        raise ValueError("MusicXML contains no part")

    divisions = 1
    beats = 4
    beat_type = 4
    absolute_quarter = 0.0
    notes: list[RawNote] = []
    tempos: list[tuple[float, float]] = []

    for measure_index, measure in enumerate(part.findall("./measure"), 1):
        attributes = measure.find("./attributes")
        if attributes is not None:
            divisions_text = attributes.findtext("divisions")
            if divisions_text:
                divisions = int(divisions_text)
            time = attributes.find("time")
            if time is not None:
                beats = int(time.findtext("beats", "4"))
                beat_type = int(time.findtext("beat-type", "4"))

        cursor = 0
        maximum_cursor = 0
        last_start = 0
        for child in measure:
            if child.tag == "direction":
                sound = child.find("sound")
                if sound is not None and sound.get("tempo"):
                    offset = int(child.findtext("offset", "0"))
                    tempos.append(
                        (
                            absolute_quarter + (cursor + offset) / divisions,
                            float(sound.get("tempo")),
                        )
                    )
            elif child.tag == "backup":
                cursor -= int(child.findtext("duration", "0"))
            elif child.tag == "forward":
                cursor += int(child.findtext("duration", "0"))
                maximum_cursor = max(maximum_cursor, cursor)
            elif child.tag == "note":
                duration = int(child.findtext("duration", "0"))
                is_chord = child.find("chord") is not None
                start = last_start if is_chord else cursor
                if not is_chord:
                    last_start = start
                    cursor += duration
                    maximum_cursor = max(maximum_cursor, cursor)
                pitch = child.find("pitch")
                if pitch is not None and duration > 0:
                    notes.append(
                        RawNote(
                            absolute_quarter + start / divisions,
                            absolute_quarter + (start + duration) / divisions,
                            midi_value(pitch),
                            measure_index,
                        )
                    )

        nominal_quarters = beats * 4.0 / beat_type
        absolute_quarter += max(maximum_cursor / divisions, nominal_quarters)

    if not tempos:
        tempos = [(0.0, 82.0)]
    tempos.sort()
    if tempos[0][0] > 0:
        tempos.insert(0, (0.0, tempos[0][1]))
    return notes, tempos, absolute_quarter


def quarter_to_seconds(quarter: float, tempos: list[tuple[float, float]]) -> float:
    previous = 0.0
    seconds = 0.0
    bpm = tempos[0][1]
    for change, next_bpm in tempos[1:]:
        if quarter <= change:
            break
        seconds += (change - previous) * 60.0 / bpm
        previous = change
        bpm = next_bpm
    return seconds + (quarter - previous) * 60.0 / bpm


def remove_isolated_instrumental_notes(notes: list[RawNote]) -> list[RawNote]:
    """Remove sparse notes in otherwise empty measures (usually a solo guide)."""
    counts: dict[int, int] = {}
    for note in notes:
        counts[note.measure] = counts.get(note.measure, 0) + 1
    melodic_measures = {measure for measure, count in counts.items() if count >= 2}
    keep_measures = set(melodic_measures)
    for measure, count in counts.items():
        if count == 1 and (
            measure - 1 in melodic_measures or measure + 1 in melodic_measures
        ):
            keep_measures.add(measure)
    return [note for note in notes if note.measure in keep_measures]


def combine_simultaneous(notes: list[RawNote]) -> list[RawNote]:
    """Collapse accidental chord tones to the highest note at each onset."""
    grouped: dict[tuple[float, int], list[RawNote]] = {}
    for note in notes:
        grouped.setdefault((note.start_quarter, note.measure), []).append(note)
    result = []
    for group in grouped.values():
        chosen = max(group, key=lambda note: note.midi)
        result.append(
            RawNote(
                chosen.start_quarter,
                max(note.end_quarter for note in group),
                chosen.midi,
                chosen.measure,
            )
        )
    return sorted(result, key=lambda note: (note.start_quarter, note.midi))


def syllable_weight(text: str) -> int:
    words = re.findall(r"[A-Za-z]+(?:'[A-Za-z]+)?", text)
    if not words:
        # Kana are already close to mora units. Kanji are less exact, but
        # counting visible Japanese characters still gives a substantially
        # better phrase-size prior than treating every line as weight one.
        return max(
            1,
            len(re.findall(r"[\u3040-\u30ff\u3400-\u9fff]", text)),
        )
    count = 0
    for word in words:
        cleaned = word.lower().replace("'", "")
        groups = re.findall(r"[aeiouy]+", cleaned)
        estimate = max(1, len(groups))
        if cleaned.endswith("e") and not cleaned.endswith(("le", "ye")) and estimate > 1:
            estimate -= 1
        count += estimate
    return max(1, count)


def partition_notes(
    notes: list[dict], lyrics: list[str]
) -> list[tuple[int, int]]:
    """Partition ordered notes into lyric lines with dynamic programming."""
    n = len(notes)
    m = len(lyrics)
    weights = [syllable_weight(line) for line in lyrics]
    scale = n / max(1, sum(weights))
    infinity = float("inf")
    dp = [[infinity] * (n + 1) for _ in range(m + 1)]
    previous = [[-1] * (n + 1) for _ in range(m + 1)]
    dp[0][0] = 0.0

    for line_index in range(m):
        expected = max(1.0, weights[line_index] * scale)
        minimum = max(1, int(expected * 0.45))
        maximum = max(minimum, int(expected * 1.85) + 2)
        remaining_lines = m - line_index - 1
        for start in range(n + 1):
            if not math.isfinite(dp[line_index][start]):
                continue
            lower = start + minimum
            upper = min(n - remaining_lines, start + maximum)
            for end in range(lower, upper + 1):
                count_cost = ((end - start - expected) / max(expected, 1.0)) ** 2
                gap = (
                    notes[end]["start_sec"] - notes[end - 1]["end_sec"]
                    if end < n
                    else 1.5
                )
                boundary_reward = min(max(gap, 0.0), 1.5) * 1.8
                cost = dp[line_index][start] + count_cost - boundary_reward
                if cost < dp[line_index + 1][end]:
                    dp[line_index + 1][end] = cost
                    previous[line_index + 1][end] = start

    end = n
    if previous[m][end] < 0:
        raise ValueError(f"Could not partition {n} notes into {m} lyric lines")
    spans = []
    for line_index in range(m, 0, -1):
        start = previous[line_index][end]
        spans.append((start, end))
        end = start
    return list(reversed(spans))


def lyric_character_spans(text: str, count: int) -> list[tuple[int, int, str]]:
    tokens = list(re.finditer(r"[A-Za-z]+(?:'[A-Za-z]+)?", text))
    if not tokens:
        tokens = list(
            re.finditer(r"[\u3040-\u30ff\u3400-\u9fff]", text)
        )
    if not tokens:
        return [(0, len(text), text)] * count
    spans = []
    for index in range(count):
        token_index = min(len(tokens) - 1, index * len(tokens) // count)
        token = tokens[token_index]
        spans.append((token.start(), token.end(), token.group(0)))
    return spans


def render_guide(notes: list[dict], output: Path, sample_rate: int = 48_000) -> None:
    duration = max(note["end_sec"] for note in notes) + 0.5
    audio = np.zeros(round(duration * sample_rate), dtype=np.float32)
    for note in notes:
        start = round(note["start_sec"] * sample_rate)
        end = round(note["end_sec"] * sample_rate)
        if end <= start:
            continue
        time = np.arange(end - start, dtype=np.float32) / sample_rate
        frequency = 440.0 * 2.0 ** ((note["midi"] - 69) / 12.0)
        tone = np.sin(2.0 * np.pi * frequency * time) * 0.16
        fade = min(round(0.015 * sample_rate), len(tone) // 2)
        if fade:
            ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
            tone[:fade] *= ramp
            tone[-fade:] *= ramp[::-1]
        audio[start:end] += tone
    output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(output, np.clip(audio, -0.95, 0.95), sample_rate, subtype="PCM_16")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--musicxml", type=Path, required=True)
    parser.add_argument("--lyrics", type=Path, required=True)
    parser.add_argument("--score-json", type=Path, required=True)
    parser.add_argument("--guide-wav", type=Path, required=True)
    parser.add_argument("--song", default="dont_look_back_in_anger")
    parser.add_argument(
        "--prepend-rest-measures",
        type=float,
        default=0.0,
        help="Insert this many 4/4 measures before the first MusicXML note.",
    )
    parser.add_argument(
        "--keep-leading-notes",
        action="store_true",
        help="Do not auto-remove a short pickup before an early long rest.",
    )
    args = parser.parse_args()

    raw_notes, tempos, _ = parse_musicxml(args.musicxml)
    raw_notes = combine_simultaneous(remove_isolated_instrumental_notes(raw_notes))
    rest_quarters = max(0.0, args.prepend_rest_measures) * 4.0
    if rest_quarters:
        raw_notes = [
            RawNote(
                note.start_quarter + rest_quarters,
                note.end_quarter + rest_quarters,
                note.midi,
                note.measure,
            )
            for note in raw_notes
        ]
    notes = [
        {
            "note_idx": index,
            "start_sec": round(quarter_to_seconds(note.start_quarter, tempos), 6),
            "end_sec": round(quarter_to_seconds(note.end_quarter, tempos), 6),
            "midi": note.midi,
            "measure": note.measure,
        }
        for index, note in enumerate(raw_notes)
    ]
    # Some melody exports prepend a short instrumental/pickup phrase before a
    # conspicuously long rest.  It is not part of the supplied lyric text.
    # Drop that prefix and make the first sung note score-time zero.
    leading_cut = 0
    for index in range(1, min(len(notes), 40)):
        gap = notes[index]["start_sec"] - notes[index - 1]["end_sec"]
        if gap >= 2.5 and notes[index]["start_sec"] <= 15.0:
            leading_cut = index
            break
    if leading_cut and not args.keep_leading_notes:
        notes = notes[leading_cut:]
    time_origin = 0.0 if args.keep_leading_notes else notes[0]["start_sec"]
    for index, note in enumerate(notes):
        note["note_idx"] = index
        note["start_sec"] = round(note["start_sec"] - time_origin, 6)
        note["end_sec"] = round(note["end_sec"] - time_origin, 6)
    lyrics = [
        line.strip()
        for line in args.lyrics.read_text(encoding="utf-8-sig").splitlines()
        if line.strip()
    ]
    partitions = partition_notes(notes, lyrics)
    lines = []
    for line_index, ((start, end), text) in enumerate(zip(partitions, lyrics)):
        line_notes = notes[start:end]
        character_spans = lyric_character_spans(text, len(line_notes))
        enriched = []
        for note, (char_start, char_end, token) in zip(
            line_notes, character_spans
        ):
            enriched.append(
                {
                    **note,
                    "char_start": char_start,
                    "char_end": char_end,
                    "kanji": token,
                    "phonemes": "",
                }
            )
        lines.append(
            {
                "line_idx": line_index,
                "text": text,
                "start_sec": enriched[0]["start_sec"],
                "end_sec": enriched[-1]["end_sec"],
                "notes": enriched,
            }
        )

    score = {
        "schema_version": 1,
        "song": args.song,
        "track": args.musicxml.stem,
        "bpm": float(tempos[0][1]),
        "tempo_events": [
            {"quarter": round(quarter, 6), "bpm": round(bpm, 6)}
            for quarter, bpm in tempos
        ],
        "lines": lines,
    }
    args.score_json.parent.mkdir(parents=True, exist_ok=True)
    args.score_json.write_text(
        json.dumps(score, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    render_guide(notes, args.guide_wav)
    print(
        f"{args.score_json}: {len(lines)} lines, {len(notes)} notes; "
        f"guide={args.guide_wav}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
