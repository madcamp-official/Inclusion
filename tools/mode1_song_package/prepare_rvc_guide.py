"""Adapt a source vocal's range and expression before RVC conversion."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import pyworld
import soundfile as sf
from scipy.ndimage import median_filter
from scipy.signal import butter, sosfiltfilt, welch


FRAME_PERIOD_MS = 5.0


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    audio, sr = sf.read(path, always_2d=True, dtype="float64")
    return np.ascontiguousarray(audio.mean(axis=1)), sr


def world_analysis(audio: np.ndarray, sr: int):
    f0, times = pyworld.harvest(
        audio, sr, f0_floor=55.0, f0_ceil=1100.0,
        frame_period=FRAME_PERIOD_MS,
    )
    f0 = pyworld.stonemask(audio, f0, times, sr)
    spectrum = pyworld.cheaptrick(audio, f0, times, sr)
    aperiodicity = pyworld.d4c(audio, f0, times, sr)
    return f0, times, spectrum, aperiodicity


def robust_pitch_stats(f0: np.ndarray) -> dict:
    voiced = f0[f0 > 0]
    midi = librosa.hz_to_midi(voiced)
    q = np.percentile(midi, [2, 10, 50, 90, 98])
    return {
        "midi_p02": round(float(q[0]), 3),
        "midi_safe_low": round(float(q[1]), 3),
        "midi_median": round(float(q[2]), 3),
        "midi_safe_high": round(float(q[3]), 3),
        "midi_p98": round(float(q[4]), 3),
        "note_safe_low": librosa.midi_to_note(q[1], unicode=False),
        "note_median": librosa.midi_to_note(q[2], unicode=False),
        "note_safe_high": librosa.midi_to_note(q[3], unicode=False),
        "voiced_frames": int(len(voiced)),
    }


def rms_curve(audio: np.ndarray, sr: int, frames: int) -> np.ndarray:
    hop = max(1, round(sr * FRAME_PERIOD_MS / 1000.0))
    rms = librosa.feature.rms(
        y=audio.astype(np.float32), frame_length=2048, hop_length=hop
    )[0]
    x_old = np.linspace(0.0, 1.0, len(rms))
    return np.interp(np.linspace(0.0, 1.0, frames), x_old, rms)


def vibrato_stats(f0: np.ndarray) -> dict:
    voiced = f0 > 0
    if voiced.sum() < 200:
        return {"rate_hz": 5.5, "depth_cents_rms": 18.0, "confidence": 0.0}
    log_pitch = np.zeros_like(f0)
    indices = np.flatnonzero(voiced)
    log_pitch[:] = np.interp(np.arange(len(f0)), indices, 1200.0 * np.log2(f0[indices]))
    trend = median_filter(log_pitch, size=61, mode="nearest")
    residual = log_pitch - trend
    sos = butter(3, [4.0, 8.0], btype="bandpass", fs=200.0, output="sos")
    band = sosfiltfilt(sos, residual)
    frequencies, power = welch(band[voiced], fs=200.0, nperseg=1024)
    mask = (frequencies >= 4.0) & (frequencies <= 8.0)
    rate = float(frequencies[mask][np.argmax(power[mask])])
    depth = float(np.sqrt(np.mean(np.square(band[voiced]))))
    confidence = float(
        np.sum(power[mask]) / max(np.sum(power[(frequencies >= 1) & (frequencies <= 12)]), 1e-9)
    )
    return {
        "rate_hz": round(rate, 3),
        "depth_cents_rms": round(float(np.clip(depth, 8.0, 30.0)), 3),
        "confidence": round(float(np.clip(confidence, 0.0, 1.0)), 3),
    }


def energy_stats(audio: np.ndarray, sr: int) -> dict:
    rms = librosa.feature.rms(y=audio.astype(np.float32))[0]
    active = rms[rms > max(float(np.max(rms)) * 0.02, 1e-5)]
    db = librosa.amplitude_to_db(active, ref=1.0)
    q = np.percentile(db, [10, 50, 90])
    return {
        "db_p10": round(float(q[0]), 3),
        "db_median": round(float(q[1]), 3),
        "db_p90": round(float(q[2]), 3),
        "dynamic_range_db": round(float(q[2] - q[0]), 3),
    }


def select_key_shift(user: dict, source: dict) -> tuple[int, list[dict]]:
    candidates = []
    # Female-to-low-male arrangements can require more than one octave.
    # The package transposes the guitar chord reference by the same amount.
    for shift in range(-18, 13):
        low = source["midi_safe_low"] + shift
        high = source["midi_safe_high"] + shift
        below = max(0.0, user["midi_safe_low"] - low)
        above = max(0.0, high - user["midi_safe_high"])
        centre = abs(source["midi_median"] + shift - user["midi_median"])
        score = 4.0 * (below * below + above * above) + centre + 0.04 * abs(shift)
        candidates.append({
            "shift": shift,
            "score": round(score, 4),
            "below_safe_range_st": round(below, 3),
            "above_safe_range_st": round(above, 3),
        })
    candidates.sort(key=lambda item: item["score"])
    return int(candidates[0]["shift"]), candidates[:5]


def voiced_segments(voiced: np.ndarray) -> list[tuple[int, int]]:
    edges = np.diff(np.pad(voiced.astype(np.int8), (1, 1)))
    return list(zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)))


def adapt_f0(
    f0: np.ndarray,
    shift: int,
    vibrato: dict,
) -> np.ndarray:
    voiced = f0 > 0
    result = np.zeros_like(f0)
    indices = np.flatnonzero(voiced)
    cents = np.interp(
        np.arange(len(f0)), indices, 1200.0 * np.log2(f0[indices])
    )
    # Suppress fast source-singer fluctuations while preserving note motion.
    trend = median_filter(cents, size=31, mode="nearest")
    softened = trend + 0.18 * (cents - trend) + shift * 100.0
    rate = vibrato["rate_hz"]
    depth = vibrato["depth_cents_rms"] * np.sqrt(2.0)
    if vibrato["confidence"] < 0.12:
        depth = 14.0
    for start, end in voiced_segments(voiced):
        if end - start < 100:
            continue
        local = np.arange(end - start) / 200.0
        fade = np.clip((local - 0.35) / 0.25, 0.0, 1.0)
        softened[start:end] += depth * fade * np.sin(2.0 * np.pi * rate * local)
    result[voiced] = 2.0 ** (softened[voiced] / 1200.0)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--user-voice", type=Path, required=True)
    parser.add_argument("--source-vocal", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--key-shift", type=int)
    args = parser.parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    user_audio, user_sr = read_mono(args.user_voice)
    source_audio, source_sr = read_mono(args.source_vocal)
    user_f0, _, _, _ = world_analysis(user_audio, user_sr)
    source_f0, _, source_sp, source_ap = world_analysis(source_audio, source_sr)
    user_pitch = robust_pitch_stats(user_f0)
    source_pitch = robust_pitch_stats(source_f0)
    user_vibrato = vibrato_stats(user_f0)
    user_energy = energy_stats(user_audio, user_sr)
    source_energy = energy_stats(source_audio, source_sr)
    recommended_shift, candidates = select_key_shift(user_pitch, source_pitch)
    key_shift = args.key_shift if args.key_shift is not None else recommended_shift

    adapted_f0 = adapt_f0(source_f0, key_shift, user_vibrato)
    prepared = pyworld.synthesize(
        adapted_f0, source_sp, source_ap, source_sr,
        frame_period=FRAME_PERIOD_MS,
    )

    source_rms = rms_curve(source_audio, source_sr, len(source_f0))
    source_db = librosa.amplitude_to_db(np.maximum(source_rms, 1e-7), ref=1.0)
    source_dynamic = max(source_energy["dynamic_range_db"], 1.0)
    user_dynamic = max(user_energy["dynamic_range_db"], 1.0)
    ratio = float(np.clip(user_dynamic / source_dynamic, 0.45, 1.0))
    target_db = source_energy["db_median"] + (
        source_db - source_energy["db_median"]
    ) * ratio
    adapted_midi = np.zeros_like(adapted_f0)
    voiced = adapted_f0 > 0
    adapted_midi[voiced] = librosa.hz_to_midi(adapted_f0[voiced])
    high_excess = np.maximum(0.0, adapted_midi - user_pitch["midi_safe_high"])
    target_db -= np.minimum(6.0, high_excess * 0.8)
    gain_frames = librosa.db_to_amplitude(target_db - source_db)
    gain_samples = np.interp(
        np.linspace(0.0, 1.0, len(prepared)),
        np.linspace(0.0, 1.0, len(gain_frames)),
        gain_frames,
    )
    prepared *= np.clip(gain_samples, 0.35, 1.8)
    peak = float(np.max(np.abs(prepared)))
    if peak > 0.98:
        prepared *= 0.98 / peak

    prepared_path = output_dir / "rvc_input_style_adapted.wav"
    sf.write(prepared_path, prepared, source_sr, subtype="PCM_24")
    profile = {
        "schema_version": 1,
        "source_file": str(args.user_voice.resolve()),
        "pitch": user_pitch,
        "vibrato": user_vibrato,
        "energy": user_energy,
    }
    warnings = []
    shifted_low = source_pitch["midi_safe_low"] + key_shift
    shifted_high = source_pitch["midi_safe_high"] + key_shift
    if shifted_low < user_pitch["midi_safe_low"]:
        warnings.append({
            "type": "below_comfortable_range",
            "semitones": round(user_pitch["midi_safe_low"] - shifted_low, 2),
        })
    if shifted_high > user_pitch["midi_safe_high"]:
        warnings.append({
            "type": "above_comfortable_range",
            "semitones": round(shifted_high - user_pitch["midi_safe_high"], 2),
        })
    plan = {
        "schema_version": 1,
        "source_vocal": str(args.source_vocal.resolve()),
        "prepared_rvc_input": str(prepared_path),
        "source_pitch": source_pitch,
        "base_key_shift": key_shift,
        "recommended_base_key_shift": recommended_shift,
        "candidate_shifts": candidates,
        "pitch_processing": {
            "source_micro_expression_retained_ratio": 0.18,
            "user_vibrato_applied": True,
        },
        "energy_processing": {
            "dynamic_range_ratio": round(ratio, 3),
            "high_note_reduction_db_per_semitone": 0.8,
            "maximum_high_note_reduction_db": 6.0,
        },
        "range_warnings": warnings,
    }
    (output_dir / "voice_profile.json").write_text(
        json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (output_dir / "style_plan.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    np.savetxt(
        output_dir / "adapted_f0.csv",
        np.column_stack([
            np.arange(len(adapted_f0)) * FRAME_PERIOD_MS / 1000.0,
            source_f0,
            adapted_f0,
        ]),
        delimiter=",",
        header="time_sec,source_f0_hz,adapted_f0_hz",
        comments="",
    )
    print(json.dumps({
        "prepared_rvc_input": str(prepared_path),
        "voice_profile": str(output_dir / "voice_profile.json"),
        "style_plan": str(output_dir / "style_plan.json"),
        "base_key_shift": key_shift,
        "warnings": warnings,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
