"""Adapt a source vocal's range and expression before RVC conversion."""

from __future__ import annotations

import argparse
import json
from math import gcd
from pathlib import Path

import librosa
import numpy as np
import pyworld
import soundfile as sf
from scipy.ndimage import median_filter
from scipy.signal import butter, resample_poly, sosfiltfilt, welch


FRAME_PERIOD_MS = 5.0


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    audio, sr = sf.read(path, always_2d=True, dtype="float64")
    return np.ascontiguousarray(audio.mean(axis=1)), sr


def world_analysis(audio: np.ndarray, sr: int):
    f0, times = world_pitch_analysis(audio, sr)
    spectrum = pyworld.cheaptrick(audio, f0, times, sr)
    aperiodicity = pyworld.d4c(audio, f0, times, sr)
    return f0, times, spectrum, aperiodicity


def world_pitch_analysis(audio: np.ndarray, sr: int):
    f0, times = pyworld.harvest(
        audio, sr, f0_floor=55.0, f0_ceil=1100.0,
        frame_period=FRAME_PERIOD_MS,
    )
    f0 = pyworld.stonemask(audio, f0, times, sr)
    return f0, times


def world_pitch_analysis_fast(audio: np.ndarray, sr: int):
    f0, times = pyworld.dio(
        audio,
        sr,
        f0_floor=55.0,
        f0_ceil=1100.0,
        frame_period=10.0,
        speed=4,
        allowed_range=0.2,
    )
    f0 = pyworld.stonemask(audio, f0, times, sr)
    return f0, times


def downsample_for_pitch(
    audio: np.ndarray, sr: int, target_sr: int = 16000
) -> tuple[np.ndarray, int]:
    if sr <= target_sr:
        return audio, sr
    divisor = gcd(sr, target_sr)
    reduced = resample_poly(audio, target_sr // divisor, sr // divisor)
    return np.ascontiguousarray(reduced, dtype=np.float64), target_sr


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


def select_key_shift(
    user: dict,
    source: dict,
    shifts: range | tuple[int, ...] = range(-18, 13),
) -> tuple[int, list[dict]]:
    candidates = []
    for shift in shifts:
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

def resolve_key_shift(
    explicit_shift: int | None,
    recommended_shift: int,
) -> int:
    """Keep RVC in the song's original key unless explicitly overridden."""
    del recommended_shift  # Retained in reports as an advisory range warning.
    return 0 if explicit_shift is None else int(explicit_shift)


def voiced_segments(voiced: np.ndarray) -> list[tuple[int, int]]:
    edges = np.diff(np.pad(voiced.astype(np.int8), (1, 1)))
    return list(zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)))


def adapt_f0(
    f0: np.ndarray,
    shift: int,
    vibrato: dict,
    source_expression_ratio: float,
    user_vibrato_mix: float,
) -> np.ndarray:
    voiced = f0 > 0
    result = np.zeros_like(f0)
    indices = np.flatnonzero(voiced)
    cents = np.interp(
        np.arange(len(f0)), indices, 1200.0 * np.log2(f0[indices])
    )
    # Suppress fast source-singer fluctuations while preserving note motion.
    trend = median_filter(cents, size=31, mode="nearest")
    softened = (
        trend
        + source_expression_ratio * (cents - trend)
        + shift * 100.0
    )
    rate = vibrato["rate_hz"]
    depth = vibrato["depth_cents_rms"] * np.sqrt(2.0)
    if vibrato["confidence"] < 0.12:
        depth = 14.0
    for start, end in voiced_segments(voiced):
        if end - start < 100:
            continue
        local = np.arange(end - start) / 200.0
        fade = np.clip((local - 0.35) / 0.25, 0.0, 1.0)
        softened[start:end] += (
            depth
            * user_vibrato_mix
            * fade
            * np.sin(2.0 * np.pi * rate * local)
        )
    result[voiced] = 2.0 ** (softened[voiced] / 1200.0)
    return result


def style_parameters(strength: int) -> dict:
    value = float(np.clip(strength, 0, 100))
    if value <= 25.0:
        source_expression = 0.05 + (0.18 - 0.05) * value / 25.0
        user_vibrato_mix = 1.0
        high_note_reduction = 1.0 + 0.2 * (25.0 - value) / 25.0
    else:
        source_mix = (value - 25.0) / 75.0
        source_expression = 0.18 + (1.0 - 0.18) * source_mix
        user_vibrato_mix = 1.0 - source_mix
        high_note_reduction = 1.0 - source_mix
    return {
        "strength": int(value),
        "source_micro_expression_retained_ratio": round(source_expression, 4),
        "user_vibrato_mix": round(user_vibrato_mix, 4),
        "high_note_reduction_mix": round(high_note_reduction, 4),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--user-voice", type=Path, required=True)
    parser.add_argument("--source-vocal", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--key-shift",
        type=int,
        help=(
            "Explicit RVC pitch shift in semitones. Defaults to 0 (the "
            "source song's original key); range analysis is advisory only."
        ),
    )
    parser.add_argument(
        "--direct-rvc",
        action="store_true",
        help=(
            "Analyse pitch at 16 kHz and let RVC apply the selected key directly. "
            "Skips WORLD spectrum decomposition, resynthesis, and expression variants."
        ),
    )
    parser.add_argument(
        "--style-strengths",
        default="100",
        help="Comma-separated source-expression strengths, e.g. 0,25,50,75,100",
    )
    args = parser.parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    user_audio, user_sr = read_mono(args.user_voice)
    source_audio, source_sr = read_mono(args.source_vocal)
    # The user recording is only used to choose the best range and expression
    # profile. Avoid the expensive spectral/aperiodicity decomposition that is
    # only needed to resynthesise the source vocal.
    user_pitch_audio, user_pitch_sr = (
        downsample_for_pitch(user_audio, user_sr)
        if args.direct_rvc
        else (user_audio, user_sr)
    )
    pitch_analyser = (
        world_pitch_analysis_fast
        if args.direct_rvc
        else world_pitch_analysis
    )
    user_f0, _ = pitch_analyser(user_pitch_audio, user_pitch_sr)
    if args.direct_rvc:
        source_pitch_audio, source_pitch_sr = downsample_for_pitch(
            source_audio, source_sr
        )
        source_f0, _ = pitch_analyser(
            source_pitch_audio, source_pitch_sr
        )
        source_sp = source_ap = None
    else:
        source_f0, _, source_sp, source_ap = world_analysis(
            source_audio, source_sr
        )
    user_pitch = robust_pitch_stats(user_f0)
    source_pitch = robust_pitch_stats(source_f0)
    user_vibrato = vibrato_stats(user_f0)
    user_energy = energy_stats(user_audio, user_sr)
    source_energy = energy_stats(source_audio, source_sr)
    # Direct mode chooses one of the song's musically vetted anchors. Recorded
    # speech is lower and narrower than a person's singing range, so allowing an
    # unconstrained search can otherwise produce an unusable -18 semitone result.
    candidate_shifts = (-12, -6, 0) if args.direct_rvc else range(-18, 13)
    recommended_shift, candidates = select_key_shift(
        user_pitch, source_pitch, candidate_shifts
    )
    key_shift = resolve_key_shift(args.key_shift, recommended_shift)

    source_rms = rms_curve(source_audio, source_sr, len(source_f0))
    source_db = librosa.amplitude_to_db(np.maximum(source_rms, 1e-7), ref=1.0)
    source_dynamic = max(source_energy["dynamic_range_db"], 1.0)
    user_dynamic = max(user_energy["dynamic_range_db"], 1.0)
    ratio = float(np.clip(user_dynamic / source_dynamic, 0.45, 1.0))
    target_db = source_energy["db_median"] + (
        source_db - source_energy["db_median"]
    ) * ratio
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
    (output_dir / "voice_profile.json").write_text(
        json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    strengths = sorted({
        int(item.strip())
        for item in args.style_strengths.split(",")
        if item.strip()
    })
    if args.direct_rvc:
        # Direct RVC still renders a single variant, but that variant must
        # honour the requested expression strength. Previously this branch
        # silently forced 25 even when the caller requested 100.
        strength = strengths[-1]
        plan_path = output_dir / f"style_plan_{strength:03d}.json"
        plan = {
            "schema_version": 2,
            "style_strength": strength,
            "source_vocal": str(args.source_vocal.resolve()),
            "prepared_rvc_input": str(args.source_vocal.resolve()),
            "source_pitch": source_pitch,
            "base_key_shift": key_shift,
            "recommended_base_key_shift": recommended_shift,
            "candidate_shifts": candidates,
            "pitch_processing": {
                "mode": "direct_rvc_original_key",
                "rvc_pitch_shift": key_shift,
            },
            "energy_processing": {"mode": "disabled"},
            "range_warnings": warnings,
        }
        plan_text = json.dumps(plan, ensure_ascii=False, indent=2) + "\n"
        plan_path.write_text(plan_text, encoding="utf-8")
        (output_dir / "style_plan.json").write_text(
            plan_text, encoding="utf-8"
        )
        variants = [{
            "strength": strength,
            "prepared_rvc_input": str(args.source_vocal.resolve()),
            "style_plan": str(plan_path),
            "rvc_pitch_shift": key_shift,
        }]
        manifest_path = output_dir / "style_variants.json"
        manifest_path.write_text(
            json.dumps({
                "schema_version": 1,
                "default_strength": strength,
                "base_key_shift": key_shift,
                "render_mode": "direct_rvc_original_key",
                "variants": variants,
            }, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(json.dumps({
            "voice_profile": str(output_dir / "voice_profile.json"),
            "manifest": str(manifest_path),
            "variants": variants,
            "base_key_shift": key_shift,
            "warnings": warnings,
        }, ensure_ascii=False, indent=2))
        return

    variants = []
    for strength in strengths:
        parameters = style_parameters(strength)
        adapted_f0 = adapt_f0(
            source_f0,
            key_shift,
            user_vibrato,
            parameters["source_micro_expression_retained_ratio"],
            parameters["user_vibrato_mix"],
        )
        prepared = pyworld.synthesize(
            adapted_f0, source_sp, source_ap, source_sr,
            frame_period=FRAME_PERIOD_MS,
        )
        adapted_midi = np.zeros_like(adapted_f0)
        voiced = adapted_f0 > 0
        adapted_midi[voiced] = librosa.hz_to_midi(adapted_f0[voiced])
        high_excess = np.maximum(
            0.0, adapted_midi - user_pitch["midi_safe_high"]
        )
        target_variant_db = target_db - np.minimum(
            6.0,
            high_excess
            * 0.8
            * parameters["high_note_reduction_mix"],
        )
        gain_frames = librosa.db_to_amplitude(target_variant_db - source_db)
        gain_samples = np.interp(
            np.linspace(0.0, 1.0, len(prepared)),
            np.linspace(0.0, 1.0, len(gain_frames)),
            gain_frames,
        )
        prepared *= np.clip(gain_samples, 0.35, 1.8)
        peak = float(np.max(np.abs(prepared)))
        if peak > 0.98:
            prepared *= 0.98 / peak

        prepared_path = output_dir / f"rvc_input_style_{strength:03d}.wav"
        plan_path = output_dir / f"style_plan_{strength:03d}.json"
        f0_path = output_dir / f"adapted_f0_{strength:03d}.csv"
        sf.write(prepared_path, prepared, source_sr, subtype="PCM_24")
        plan = {
            "schema_version": 2,
            "style_strength": strength,
            "source_vocal": str(args.source_vocal.resolve()),
            "prepared_rvc_input": str(prepared_path),
            "source_pitch": source_pitch,
            "base_key_shift": key_shift,
            "recommended_base_key_shift": recommended_shift,
            "candidate_shifts": candidates,
            "pitch_processing": parameters,
            "energy_processing": {
                "dynamic_range_ratio": round(ratio, 3),
                "high_note_reduction_db_per_semitone": 0.8,
                "maximum_high_note_reduction_db": 6.0,
            },
            "range_warnings": warnings,
        }
        plan_path.write_text(
            json.dumps(plan, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        np.savetxt(
            f0_path,
            np.column_stack([
                np.arange(len(adapted_f0)) * FRAME_PERIOD_MS / 1000.0,
                source_f0,
                adapted_f0,
            ]),
            delimiter=",",
            header="time_sec,source_f0_hz,adapted_f0_hz",
            comments="",
        )
        variants.append({
            "strength": strength,
            "prepared_rvc_input": str(prepared_path),
            "style_plan": str(plan_path),
        })
        if strength == 25:
            sf.write(
                output_dir / "rvc_input_style_adapted.wav",
                prepared,
                source_sr,
                subtype="PCM_24",
            )
            (output_dir / "style_plan.json").write_text(
                json.dumps(plan, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
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
    manifest_path = output_dir / "style_variants.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 1,
            "default_strength": strengths[-1],
            "base_key_shift": key_shift,
            "variants": variants,
        }, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "voice_profile": str(output_dir / "voice_profile.json"),
        "manifest": str(manifest_path),
        "variants": variants,
        "base_key_shift": key_shift,
        "warnings": warnings,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
