"""Build a Mode 1 song package from score metadata and aligned vocal audio.

The score JSON is timed against a synthesized guide vocal.  This tool aligns
that guide to the isolated original vocal with chroma DTW, transfers every
lyric-line boundary to source-audio time, attaches the source-aligned chord
events, and cuts an already voice-converted vocal into phrase files.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

from chord_cleanup import (
    clean_chord_events,
    normalize_chord_name,
    transpose_chord_name,
)


ANALYSIS_SR = 16_000
HOP_LENGTH = 512


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def active_bounds(y: np.ndarray, sr: int) -> tuple[int, int]:
    """Return conservative active-audio frame bounds."""
    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=HOP_LENGTH)[0]
    if not np.any(rms > 0):
        return 0, len(rms) - 1
    threshold = max(float(np.max(rms)) * 0.025, 1e-5)
    active = np.flatnonzero(rms >= threshold)
    padding = round(1.5 * sr / HOP_LENGTH)
    return max(0, int(active[0]) - padding), min(len(rms) - 1, int(active[-1]) + padding)


def chroma_features(y: np.ndarray, sr: int) -> np.ndarray:
    harmonic = librosa.effects.harmonic(y, margin=2.0)
    chroma = librosa.feature.chroma_cens(
        y=harmonic,
        sr=sr,
        hop_length=HOP_LENGTH,
        n_chroma=12,
    )
    # Add a small onset component to help distinguish repeated melodic phrases.
    onset = librosa.onset.onset_strength(
        y=y,
        sr=sr,
        hop_length=HOP_LENGTH,
    )
    onset /= max(float(np.max(onset)), 1e-8)
    return np.vstack([chroma, onset[np.newaxis, :] * 0.25])


def build_time_map(
    guide_path: Path,
    source_path: Path,
) -> tuple[np.ndarray, np.ndarray, dict]:
    guide, _ = librosa.load(guide_path, sr=ANALYSIS_SR, mono=True)
    source, _ = librosa.load(source_path, sr=ANALYSIS_SR, mono=True)

    guide_feat = chroma_features(guide, ANALYSIS_SR)
    source_feat = chroma_features(source, ANALYSIS_SR)
    g0, g1 = active_bounds(guide, ANALYSIS_SR)
    s0, s1 = active_bounds(source, ANALYSIS_SR)

    guide_trim = guide_feat[:, g0 : g1 + 1]
    source_trim = source_feat[:, s0 : s1 + 1]
    _, path = librosa.sequence.dtw(
        X=guide_trim,
        Y=source_trim,
        metric="cosine",
        global_constraints=True,
        band_rad=0.20,
    )
    path = path[::-1]
    guide_frames = path[:, 0].astype(float) + g0
    source_frames = path[:, 1].astype(float) + s0

    # DTW may map several source frames to one guide frame. Collapse those
    # points to their median, then interpolate a continuous monotonic map.
    unique_guide = np.unique(guide_frames)
    mapped_source = np.array(
        [np.median(source_frames[guide_frames == frame]) for frame in unique_guide]
    )
    mapped_source = np.maximum.accumulate(mapped_source)
    guide_seconds = librosa.frames_to_time(
        unique_guide,
        sr=ANALYSIS_SR,
        hop_length=HOP_LENGTH,
    )
    source_seconds = librosa.frames_to_time(
        mapped_source,
        sr=ANALYSIS_SR,
        hop_length=HOP_LENGTH,
    )

    diagnostics = {
        "analysis_sample_rate": ANALYSIS_SR,
        "hop_length": HOP_LENGTH,
        "guide_duration_sec": round(len(guide) / ANALYSIS_SR, 6),
        "source_duration_sec": round(len(source) / ANALYSIS_SR, 6),
        "guide_active_start_sec": round(g0 * HOP_LENGTH / ANALYSIS_SR, 6),
        "guide_active_end_sec": round(g1 * HOP_LENGTH / ANALYSIS_SR, 6),
        "source_active_start_sec": round(s0 * HOP_LENGTH / ANALYSIS_SR, 6),
        "source_active_end_sec": round(s1 * HOP_LENGTH / ANALYSIS_SR, 6),
        "dtw_path_points": int(len(path)),
    }
    return guide_seconds, source_seconds, diagnostics


def map_time(
    score_sec: float,
    guide_seconds: np.ndarray,
    source_seconds: np.ndarray,
) -> float:
    return float(
        np.interp(
            score_sec,
            guide_seconds,
            source_seconds,
            left=source_seconds[0],
            right=source_seconds[-1],
        )
    )


def chords_for_span(
    chord_events: list[dict],
    start_sec: float,
    end_sec: float,
) -> list[dict]:
    active = None
    inside = []
    for event in chord_events:
        if event["start_sec"] <= start_sec:
            active = event
        if start_sec <= event["start_sec"] < end_sec:
            inside.append(event)
    if active is not None and (
        not inside or inside[0]["start_sec"] > start_sec + 1e-6
    ):
        return [active, *inside]
    return inside


def cut_phrase(
    audio: np.ndarray,
    sr: int,
    start_sec: float,
    end_sec: float,
    output_path: Path,
    margin_sec: float,
) -> dict:
    clip_start = max(0.0, start_sec - margin_sec)
    clip_end = min(len(audio) / sr, end_sec + margin_sec)
    start_sample = round(clip_start * sr)
    end_sample = round(clip_end * sr)
    clip = audio[start_sample:end_sample].copy()

    fade_samples = min(round(0.02 * sr), len(clip) // 2)
    if fade_samples:
        fade = np.linspace(0.0, 1.0, fade_samples, dtype=clip.dtype)
        if clip.ndim == 2:
            fade = fade[:, np.newaxis]
        clip[:fade_samples] *= fade
        clip[-fade_samples:] *= fade[::-1]

    sf.write(output_path, clip, sr, subtype="PCM_24")
    return {
        "file": output_path.name,
        "clip_start_sec": round(clip_start, 6),
        "clip_end_sec": round(clip_end, 6),
        "content_offset_sec": round(start_sec - clip_start, 6),
    }


def split_line_into_micro_spans(
    line: dict,
    minimum_sec: float = 0.55,
    maximum_sec: float = 1.20,
) -> list[dict]:
    """Group score notes into short, lyric-safe playback spans."""
    notes = line["notes"]
    if not notes:
        return [
            {
                "start_sec": float(line["start_sec"]),
                "end_sec": float(line["end_sec"]),
                "notes": [],
                "lyrics": line["text"],
            }
        ]

    spans: list[dict] = []
    group_start = 0
    for index in range(len(notes) - 1):
        first = notes[group_start]
        current = notes[index]
        following = notes[index + 1]
        duration = float(current["end_sec"]) - float(first["start_sec"])
        next_duration = float(following["end_sec"]) - float(first["start_sec"])
        gap = float(following["start_sec"]) - float(current["end_sec"])
        should_close = duration >= minimum_sec and (
            gap >= 0.09 or next_duration > maximum_sec
        )
        if should_close:
            spans.append(
                make_micro_span(line, notes[group_start : index + 1])
            )
            group_start = index + 1

    spans.append(make_micro_span(line, notes[group_start:]))

    # Avoid tiny tails by merging them into the previous span.
    if len(spans) >= 2:
        tail_duration = spans[-1]["end_sec"] - spans[-1]["start_sec"]
        if tail_duration < 0.32:
            spans[-2]["end_sec"] = spans[-1]["end_sec"]
            spans[-2]["notes"].extend(spans[-1]["notes"])
            spans[-2]["lyrics"] += spans[-1]["lyrics"]
            spans.pop()
    return spans


def make_micro_span(line: dict, notes: list[dict]) -> dict:
    start_sec = float(notes[0]["start_sec"])
    end_sec = float(notes[-1]["end_sec"])
    char_starts = [int(n.get("char_start", 0)) for n in notes]
    char_ends = [int(n.get("char_end", 0)) for n in notes]
    char_start = min(char_starts, default=0)
    char_end = max(char_ends, default=char_start)
    lyrics = line["text"][char_start:char_end].strip()
    if not lyrics:
        lyrics = "".join(str(n.get("kanji", "")) for n in notes).strip()
    return {
        "start_sec": start_sec,
        "end_sec": end_sec,
        "notes": notes,
        "lyrics": lyrics,
    }


def build_package(args: argparse.Namespace) -> Path:
    score = load_json(args.score_json)
    chords_json = load_json(args.chords_json)
    style_plan = load_json(args.style_plan_json) if args.style_plan_json else {}
    base_key_shift = int(style_plan.get("base_key_shift", 0))
    output_dir: Path = args.output_dir
    phrase_dir = output_dir / "vocals"
    micro_phrase_dir = output_dir / "micro_vocals"
    phrase_dir.mkdir(parents=True, exist_ok=True)
    micro_phrase_dir.mkdir(parents=True, exist_ok=True)

    guide_seconds, source_seconds, diagnostics = build_time_map(
        args.guide_wav,
        args.source_vocal_wav,
    )
    converted_audio, converted_sr = sf.read(args.converted_vocal_wav, always_2d=False)

    chord_events = clean_chord_events([
        {
            "start_sec": float(event["start_sec"]),
            "raw_chord": event["chord"],
            "chord": normalize_chord_name(event["chord"]),
        }
        for event in chords_json["chords"]
    ])
    for event in chord_events:
        event["original_chord"] = event["chord"]
        event["chord"] = transpose_chord_name(event["chord"], base_key_shift)

    phrases = []
    previous_end = 0.0
    for line in score["lines"]:
        source_start = map_time(
            float(line["start_sec"]),
            guide_seconds,
            source_seconds,
        )
        source_end = map_time(
            float(line["end_sec"]),
            guide_seconds,
            source_seconds,
        )
        source_start = max(previous_end, source_start)
        source_end = max(source_start + 0.05, source_end)
        previous_end = source_end

        phrase_chords = chords_for_span(
            chord_events,
            source_start,
            source_end,
        )
        filename = f"phrase_{int(line['line_idx']):03d}.wav"
        clip_metadata = cut_phrase(
            converted_audio,
            converted_sr,
            source_start,
            source_end,
            phrase_dir / filename,
            args.margin_sec,
        )
        phrases.append(
            {
                "phrase_id": f"line_{int(line['line_idx']):03d}",
                "line_idx": int(line["line_idx"]),
                "lyrics": line["text"],
                "score": {
                    "start_sec": float(line["start_sec"]),
                    "end_sec": float(line["end_sec"]),
                },
                "source": {
                    "start_sec": round(source_start, 6),
                    "end_sec": round(source_end, 6),
                },
                "notes": line["notes"],
                "chords": phrase_chords,
                "vocal": clip_metadata,
            }
        )

    micro_phrases = []
    previous_micro_end = 0.0
    micro_index = 0
    for line in score["lines"]:
        for span in split_line_into_micro_spans(line):
            source_start = map_time(
                float(span["start_sec"]),
                guide_seconds,
                source_seconds,
            )
            source_end = map_time(
                float(span["end_sec"]),
                guide_seconds,
                source_seconds,
            )
            source_start = max(previous_micro_end, source_start)
            source_end = max(source_start + 0.05, source_end)
            previous_micro_end = source_end

            micro_chords = chords_for_span(
                chord_events,
                source_start,
                source_end,
            )
            filename = f"micro_{micro_index:03d}.wav"
            clip_metadata = cut_phrase(
                converted_audio,
                converted_sr,
                source_start,
                source_end,
                micro_phrase_dir / filename,
                args.micro_margin_sec,
            )
            clip_metadata["directory"] = "micro_vocals"
            micro_phrases.append(
                {
                    "phrase_id": f"micro_{micro_index:03d}",
                    "parent_line_idx": int(line["line_idx"]),
                    "lyrics": span["lyrics"],
                    "score": {
                        "start_sec": float(span["start_sec"]),
                        "end_sec": float(span["end_sec"]),
                    },
                    "source": {
                        "start_sec": round(source_start, 6),
                        "end_sec": round(source_end, 6),
                    },
                    "notes": span["notes"],
                    "chords": micro_chords,
                    "vocal": clip_metadata,
                }
            )
            micro_index += 1

    anchor_interval = max(1, round(5.0 * ANALYSIS_SR / HOP_LENGTH))
    anchors = [
        {
            "score_sec": round(float(guide_seconds[i]), 6),
            "source_sec": round(float(source_seconds[i]), 6),
        }
        for i in range(0, len(guide_seconds), anchor_interval)
    ]
    anchors.append(
        {
            "score_sec": round(float(guide_seconds[-1]), 6),
            "source_sec": round(float(source_seconds[-1]), 6),
        }
    )

    package = {
        "schema_version": 2,
        "song": score.get("song", args.output_dir.name),
        "score_track": score.get("track"),
        "score_bpm": float(score["bpm"]),
        "chord_bpm": float(chords_json["bpm"]),
        "base_key_shift": base_key_shift,
        "vocal_style": style_plan,
        "audio": {
            "source_vocal": str(args.source_vocal_wav.resolve()),
            "converted_vocal": str(args.converted_vocal_wav.resolve()),
            "guide_vocal": str(args.guide_wav.resolve()),
            "converted_sample_rate": converted_sr,
        },
        "alignment": {
            "method": "chroma_cens_dtw",
            "diagnostics": diagnostics,
            "anchors": anchors,
        },
        "chord_timeline": chord_events,
        "phrases": phrases,
        "micro_phrases": micro_phrases,
    }
    output_path = output_dir / "song_package.json"
    output_path.write_text(
        json.dumps(package, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return output_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--score-json", type=Path, required=True)
    parser.add_argument("--chords-json", type=Path, required=True)
    parser.add_argument("--guide-wav", type=Path, required=True)
    parser.add_argument("--source-vocal-wav", type=Path, required=True)
    parser.add_argument("--converted-vocal-wav", type=Path, required=True)
    parser.add_argument("--style-plan-json", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--margin-sec", type=float, default=0.12)
    parser.add_argument("--micro-margin-sec", type=float, default=0.08)
    return parser.parse_args()


if __name__ == "__main__":
    print(build_package(parse_args()))
