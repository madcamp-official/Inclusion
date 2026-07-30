"""Does the packaged score time agree with the ORIGINAL recording's vocals?

Oracle A proves the micro clips rebuild the converted vocal. This script asks
the prior question: are the package's score.start_sec values actually where the
singer enters on the original master?

For every micro phrase that follows a real rest (so the entry is unambiguous)
we look for the first vocal energy in the separated original vocal inside a
window around score.start_sec, and report the signed error.

Usage:
  python oracle_a_score_vs_original.py <song_package.json> <output-dir>
"""
from __future__ import annotations

import csv
import os
import statistics
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_common import SR, load_package, read_audio, rms_envelope

# Only phrases preceded by at least this much scored rest give an unambiguous
# entry; contiguous phrases have the previous syllable still ringing.
MIN_REST_SEC = 0.45
SEARCH_BACK = 0.45
SEARCH_FWD = 0.60


VOWEL_FRAC = 0.50   # the syllable nucleus is loud
ATTACK_FRAC = 0.02  # the foot of the consonant ramp


def entry_time(env_t, env_r, floor, center):
    """Locate the syllable nucleus and the foot of its attack.

    Sung entries ramp in over ~100-200 ms, so a single absolute threshold
    either latches onto breath/bleed or misses the consonant entirely.
    Instead the nucleus is found as a fraction of the local peak, then the
    envelope is walked backwards to where the rise actually began.

    Returns (attack_sec, vowel_sec) or (None, None).
    """
    lo = np.searchsorted(env_t, center - SEARCH_BACK)
    hi = np.searchsorted(env_t, center + SEARCH_FWD)
    if hi - lo < 20:
        return None, None
    seg = env_r[lo:hi]
    peak = float(np.max(seg))
    if peak <= floor:
        return None, None

    hits = np.flatnonzero(seg >= peak * VOWEL_FRAC)
    if hits.size == 0:
        return None, None
    v = int(hits[0])

    foot = max(peak * ATTACK_FRAC, floor * 0.5)
    a = v
    while a > 0 and seg[a - 1] >= foot:
        a -= 1
    return float(env_t[lo + a]), float(env_t[lo + v])


def main(package_path: str, out_dir: str):
    pkg = load_package(package_path)
    micro = pkg["micro_phrases"]

    sep, _ = read_audio(pkg["audio"]["source_vocal"])
    conv, _ = read_audio(pkg["audio"]["converted_vocal"])

    st, sr_env = rms_envelope(sep, SR, win_ms=20.0, hop_ms=1.0)
    ct, cr_env = rms_envelope(conv, SR, win_ms=20.0, hop_ms=1.0)

    # noise floor: well below the active-singing level
    sep_floor = float(np.percentile(sr_env[sr_env > 0], 60)) * 0.25
    conv_floor = float(np.percentile(cr_env[cr_env > 0], 60)) * 0.25

    rows = []
    for i, m in enumerate(micro):
        score_start = float(m["score"]["start_sec"])
        prev_end = float(micro[i - 1]["score"]["end_sec"]) if i else 0.0
        rest = score_start - prev_end
        if i > 0 and rest < MIN_REST_SEC:
            continue
        sa, sv = entry_time(st, sr_env, sep_floor, score_start)
        ca, cv = entry_time(ct, cr_env, conv_floor, score_start)
        content_off = float(m["vocal"]["content_offset_sec"])

        def ms(x):
            return round((x - score_start) * 1000.0, 1) if x else ""

        rows.append({
            "index": i,
            "phrase_id": m["phrase_id"],
            "lyrics": m["lyrics"],
            "preceding_rest_sec": round(rest, 4),
            "score_start_sec": round(score_start, 4),
            "content_offset_sec": content_off,
            "separated_attack_sec": round(sa, 4) if sa else "",
            "separated_vowel_sec": round(sv, 4) if sv else "",
            "separated_attack_error_ms": ms(sa),
            "separated_vowel_error_ms": ms(sv),
            "converted_attack_sec": round(ca, 4) if ca else "",
            "converted_vowel_sec": round(cv, 4) if cv else "",
            "converted_attack_error_ms": ms(ca),
            "converted_vowel_error_ms": ms(cv),
            # how much of the attack falls outside the clip's lead-in pad
            "attack_lead_beyond_pad_ms": (
                round((-ms(ca) / 1000.0 - content_off) * 1000.0, 1)
                if ca and ms(ca) != "" and ms(ca) < 0 else 0.0),
        })

    path = os.path.join(out_dir, "score_vs_original_entries.csv")
    os.makedirs(out_dir, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    def stats(key):
        v = [r[key] for r in rows if r[key] != ""]
        if not v:
            return None
        a = sorted(abs(x) for x in v)
        return {
            "n": len(v),
            "median_signed_ms": round(statistics.median(v), 1),
            "median_abs_ms": round(statistics.median(a), 1),
            "p95_abs_ms": round(a[int(0.95 * (len(a) - 1))], 1),
            "max_abs_ms": round(a[-1], 1),
            "within_60ms": sum(1 for x in a if x <= 60),
        }

    print("unambiguous entries analysed:", len(rows))
    for k in ("separated_vowel_error_ms", "separated_attack_error_ms",
              "converted_vowel_error_ms", "converted_attack_error_ms"):
        print("%-28s %s" % (k, stats(k)))
    beyond = [r["attack_lead_beyond_pad_ms"] for r in rows
              if r["attack_lead_beyond_pad_ms"]]
    if beyond:
        print("attack starting earlier than the %.0f ms pad: %d/%d, "
              "median %.0f ms, max %.0f ms"
              % (rows[0]["content_offset_sec"] * 1000, len(beyond), len(rows),
                 statistics.median(beyond), max(beyond)))
    print("wrote", path)
    return rows


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
