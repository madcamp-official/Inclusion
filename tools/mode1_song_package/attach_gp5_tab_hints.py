#!/usr/bin/env python3
"""Attach tolerant Guitar Pro note-pattern hints to a Mode 1 song package."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

import guitarpro


TICKS_PER_QUARTER = 960


def playable_chord(event: dict) -> bool:
    return event.get("chord", "").strip().upper() not in {"", "N", "N.C."}


def tempo_events(song) -> list[tuple[int, int]]:
    events = [(song.measureHeaders[0].start, int(song.tempo))]
    for track in song.tracks:
        for measure in track.measures:
            for voice in measure.voices:
                for beat in voice.beats:
                    change = getattr(beat.effect, "mixTableChange", None)
                    tempo = getattr(change, "tempo", None) if change else None
                    if tempo is not None:
                        events.append((beat.start, int(tempo.value)))
    return sorted(set(events))


def tick_to_seconds(tick: int, tempos: list[tuple[int, int]]) -> float:
    first_tick, bpm = tempos[0]
    previous_tick = first_tick
    total = 0.0
    for change_tick, next_bpm in tempos[1:]:
        if tick <= change_tick:
            break
        total += (
            (change_tick - previous_tick)
            / TICKS_PER_QUARTER
            * 60.0
            / bpm
        )
        previous_tick = change_tick
        bpm = next_bpm
    total += (
        (tick - previous_tick)
        / TICKS_PER_QUARTER
        * 60.0
        / bpm
    )
    return total


def extract_note_groups(song) -> list[dict]:
    track = max(
        song.tracks,
        key=lambda candidate: sum(
            len(beat.notes)
            for measure in candidate.measures
            for voice in measure.voices
            for beat in voice.beats
        ),
    )
    tempos = tempo_events(song)
    groups: list[dict] = []
    for measure in track.measures:
        for voice_index, voice in enumerate(measure.voices):
            for beat in voice.beats:
                if not beat.notes:
                    continue
                notes = sorted(
                    (
                        {
                            "midi": int(note.realValue),
                            "pitch_class": int(note.realValue) % 12,
                            "string": int(note.string),
                            "fret": int(note.value),
                        }
                        for note in beat.notes
                    ),
                    key=lambda note: note["string"],
                    reverse=True,
                )
                groups.append(
                    {
                        "raw_sec": tick_to_seconds(beat.start, tempos),
                        "measure": int(measure.header.number),
                        "voice": voice_index,
                        "duration_value": int(beat.duration.value),
                        "notes": notes,
                    }
                )
    groups.sort(key=lambda group: (group["raw_sec"], group["voice"]))
    return groups


def align_groups_to_audio(groups: list[dict], audio_path: Path) -> dict:
    import librosa
    import numpy as np

    sample_rate = 22_050
    hop_length = 2_048
    audio, _ = librosa.load(
        audio_path,
        sr=sample_rate,
        mono=True,
    )
    audio_chroma = librosa.feature.chroma_cqt(
        y=audio,
        sr=sample_rate,
        hop_length=hop_length,
    )
    symbolic_frames = max(
        2,
        int(
            (groups[-1]["raw_sec"] + 1.0)
            * sample_rate
            / hop_length)
        + 1,
    )
    symbolic = np.full((12, symbolic_frames), 1.0e-4, dtype=np.float32)
    for index, group in enumerate(groups):
        start = int(group["raw_sec"] * sample_rate / hop_length)
        next_time = (
            groups[index + 1]["raw_sec"]
            if index + 1 < len(groups)
            else group["raw_sec"] + 0.35
        )
        end = min(
            symbolic_frames,
            max(
                start + 1,
                int(
                    min(next_time, group["raw_sec"] + 0.45)
                    * sample_rate
                    / hop_length)
                + 1,
            ),
        )
        for note in group["notes"]:
            symbolic[note["pitch_class"], start:end] += 1.0
    symbolic = librosa.util.normalize(symbolic, axis=0)
    audio_chroma = librosa.util.normalize(audio_chroma, axis=0)
    _, path = librosa.sequence.dtw(
        X=symbolic,
        Y=audio_chroma,
        metric="cosine",
        global_constraints=True,
        band_rad=0.12,
        step_sizes_sigma=np.asarray(
            [[1, 1], [1, 2], [2, 1]],
            dtype=np.uint32,
        ),
        weights_add=np.asarray([0.0, 0.9, 0.9]),
    )
    path = path[::-1]
    buckets: list[list[int]] = [[] for _ in range(symbolic_frames)]
    for symbolic_frame, audio_frame in path:
        buckets[int(symbolic_frame)].append(int(audio_frame))
    known_symbolic = []
    known_audio = []
    for frame, matches in enumerate(buckets):
        if matches:
            known_symbolic.append(frame)
            known_audio.append(float(np.median(matches)))
    mapped_audio_frames = np.interp(
        np.arange(symbolic_frames),
        known_symbolic,
        known_audio,
    )
    for group in groups:
        symbolic_position = (
            group["raw_sec"] * sample_rate / hop_length
        )
        lower = int(np.floor(symbolic_position))
        upper = min(symbolic_frames - 1, lower + 1)
        fraction = symbolic_position - lower
        audio_frame = (
            mapped_audio_frames[lower] * (1.0 - fraction)
            + mapped_audio_frames[upper] * fraction
        )
        group["start_sec"] = round(
            audio_frame * hop_length / sample_rate,
            6,
        )
    return {
        "method": "audio-chroma-dtw",
        "reference_audio": audio_path.name,
        "sample_rate": sample_rate,
        "hop_length": hop_length,
        "path_points": int(len(path)),
        "raw_first_sec": groups[0]["raw_sec"],
        "raw_last_sec": groups[-1]["raw_sec"],
        "score_first_sec": groups[0]["start_sec"],
        "score_last_sec": groups[-1]["start_sec"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("song_package", type=Path)
    parser.add_argument("gp5_file", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        help="Defaults to updating song_package in place.",
    )
    parser.add_argument(
        "--reference-audio",
        type=Path,
        help="Align GP5 note chroma directly to the original full-song audio.",
    )
    args = parser.parse_args()

    package = json.loads(args.song_package.read_text(encoding="utf-8"))
    song = guitarpro.parse(str(args.gp5_file))
    groups = extract_note_groups(song)
    timeline = package["chord_timeline"]
    playable_indexes = [
        index for index, event in enumerate(timeline) if playable_chord(event)
    ]
    if not groups or not playable_indexes:
        raise RuntimeError("GP5 notes and a playable chord timeline are required")

    raw_first = groups[0]["raw_sec"]
    raw_last = groups[-1]["raw_sec"]
    score_first = float(timeline[playable_indexes[0]]["start_sec"])
    score_last = float(timeline[playable_indexes[-1]]["start_sec"])
    scale = (score_last - score_first) / max(1.0e-6, raw_last - raw_first)
    if args.reference_audio:
        alignment = align_groups_to_audio(groups, args.reference_audio)
        for group in groups:
            group.pop("raw_sec")
    else:
        for group in groups:
            group["start_sec"] = round(
                score_first + (group.pop("raw_sec") - raw_first) * scale,
                6,
            )
        alignment = {
            "method": "endpoint-linear-to-chord-timeline",
            "raw_first_sec": raw_first,
            "raw_last_sec": raw_last,
            "score_first_sec": score_first,
            "score_last_sec": score_last,
            "scale": scale,
        }

    hints = []
    group_cursor = 0
    for playable_position, chord_index in enumerate(playable_indexes):
        start = float(timeline[chord_index]["start_sec"])
        end = (
            float(timeline[playable_indexes[playable_position + 1]]["start_sec"])
            if playable_position + 1 < len(playable_indexes)
            else score_last + 4.0
        )
        segment_groups = []
        while group_cursor < len(groups) and groups[group_cursor]["start_sec"] < start:
            group_cursor += 1
        scan = group_cursor
        while scan < len(groups) and groups[scan]["start_sec"] < end:
            segment_groups.append(groups[scan])
            scan += 1
        group_cursor = scan

        pitch_counts: Counter[int] = Counter()
        bass_counts: Counter[int] = Counter()
        simultaneous_groups = 0
        for group in segment_groups:
            pitch_counts.update(
                note["pitch_class"] for note in group["notes"]
            )
            if group["notes"]:
                bass_counts[min(note["midi"] for note in group["notes"]) % 12] += 1
            if len(group["notes"]) >= 3:
                simultaneous_groups += 1

        pitch_mask = sum(1 << pc for pc in pitch_counts)
        bass_mask = sum(1 << pc for pc, _ in bass_counts.most_common(3))
        hints.append(
            {
                "chord_event_index": chord_index,
                "start_sec": round(start, 6),
                "end_sec": round(end, 6),
                "pitch_class_mask": pitch_mask,
                "bass_pitch_class_mask": bass_mask,
                "note_group_count": len(segment_groups),
                "simultaneous_group_count": simultaneous_groups,
                "arpeggio_likelihood": round(
                    1.0
                    - simultaneous_groups / max(1, len(segment_groups)),
                    4,
                ),
                "events": [
                    {
                        "offset_sec": round(group["start_sec"] - start, 6),
                        "measure": group["measure"],
                        "duration_value": group["duration_value"],
                        "notes": group["notes"],
                    }
                    for group in segment_groups
                ],
            }
        )

    package["tab_tracking"] = {
        "schema_version": 1,
        "source_file": args.gp5_file.name,
        "source_format": "guitar-pro-5",
        "track_name": song.tracks[0].name if song.tracks else "",
        "standard_tuning": [
            int(string.value) for string in song.tracks[0].strings
        ] if song.tracks else [],
        "raw_tempo": int(song.tempo),
        "raw_measure_count": len(song.measureHeaders),
        "raw_note_group_count": len(groups),
        "alignment": alignment,
        "chord_hints": hints,
    }

    output = args.output or args.song_package
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(package, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    nonempty = sum(bool(hint["events"]) for hint in hints)
    print(f"output={output.resolve()}")
    print(f"note_groups={len(groups)}")
    print(f"chord_hints={len(hints)}")
    print(f"nonempty_chord_hints={nonempty}")
    print(f"alignment={alignment['method']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
