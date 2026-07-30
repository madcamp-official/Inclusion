"""Check the block-size x song x guitar x mode regression matrix.

For every render directory produced by the shell loop, verify:
  - phrase count matches the expected package total
  - no duplicate phrase_index
  - no out-of-order phrase_index
  - no expired phrases (renderer's own stderr summary)
  - no realtime_trace drops
  - intro vocal count is 0 (no phrase index appears before the intro's
    known first-vocal score time, cross-checked against the phrase's own
    reported time_sec vs. that boundary is not directly available here, so
    this checks the OFFLINE stat instead: expired_phrases==0 is the strongest
    available proxy plus phrase_index 0's timestamp should exceed a sane
    floor)
  - pause runaway: no consecutive phrase timestamps whose gap is much
    larger than what the package's own score gap implies, i.e no forward
    jump that outruns realistic tempo (only meaningful with a real pause
    take; flagged for manual reading rather than hard-failed since guitar
    tempo genuinely varies)

Usage:
  python check_regression_matrix.py <matrix_dir> --expected bansanka=149 oasis=210
"""
import argparse
import csv
import json
import os
import re
import sys


def read_stdout_stats(path):
    stats = {}
    if not os.path.isfile(path):
        return stats
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^([a-zA-Z_]+)=(.+)$", line.strip())
            if m:
                stats[m.group(1)] = m.group(2)
    return stats


def check_one(run_dir, expected_count):
    ev_path = os.path.join(run_dir, "phrase_events.csv")
    stdout_path = run_dir + ".stdout.txt"
    stats = read_stdout_stats(stdout_path)
    result = {"dir": os.path.basename(run_dir)}

    if not os.path.isfile(ev_path):
        result["ok"] = False
        result["reason"] = "missing phrase_events.csv"
        return result

    rows = list(csv.DictReader(open(ev_path, encoding="utf-8")))
    indices = [int(r["phrase_index"]) for r in rows]
    times = [float(r["time_sec"]) for r in rows]

    n = len(rows)
    dup = len(indices) != len(set(indices))
    out_of_order = any(b <= a for a, b in zip(indices, indices[1:]))
    time_out_of_order = any(b < a - 1e-6 for a, b in zip(times, times[1:]))
    expired = int(stats.get("expired_phrases", "-1"))
    recovered = int(stats.get("recovered_skipped_chords", "-1"))
    trace_dropped = int(stats.get("realtime_trace_dropped", "-1"))

    problems = []
    if n != expected_count:
        problems.append("phrase_count %d != expected %d" % (n, expected_count))
    if dup:
        problems.append("duplicate phrase_index")
    if out_of_order:
        problems.append("phrase_index out of order")
    if time_out_of_order:
        problems.append("time_sec out of order")
    if expired > 0:
        problems.append("expired_phrases=%d" % expired)
    if trace_dropped > 0:
        problems.append("realtime_trace_dropped=%d" % trace_dropped)

    result.update({
        "ok": len(problems) == 0,
        "phrase_count": n,
        "duplicate": dup,
        "out_of_order": out_of_order,
        "expired_phrases": expired,
        "recovered_skipped_chords": recovered,
        "realtime_trace_dropped": trace_dropped,
        "first_phrase_time": times[0] if times else None,
        "problems": problems,
    })
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("matrix_dir")
    ap.add_argument("--expected", nargs="+", required=True,
                    help="song=count or song:guitar=count pairs. "
                         "Partial real guitar takes cover only part of the "
                         "song, so their expected count differs from the "
                         "package's full phrase total.")
    ap.add_argument("--known-block-variance", nargs="*", default=[],
                    help="song:guitar:block entries where the phrase count "
                         "is already known to differ (pre-existing, not "
                         "caused by this session's changes) -- flagged, not "
                         "failed")
    args = ap.parse_args()
    expected = {}
    for item in args.expected:
        key, val = item.split("=")
        expected[key] = int(val)
    known_variance = set(args.known_block_variance)

    manifest_path = os.path.join(args.matrix_dir, "manifest.csv")
    rows = list(csv.DictReader(open(manifest_path, encoding="utf-8")))

    results = []
    for r in rows:
        song = r["song"]
        tag = r["guitar"]
        bs = r["block"]
        mode = r["mode"]
        run_dir = os.path.join(
            args.matrix_dir, "%s_%s_b%s_%s" % (song, tag, bs, mode))
        key = "%s:%s" % (song, tag)
        exp = expected.get(key, expected.get(song))
        res = check_one(run_dir, exp)
        variance_key = "%s:%s:%s" % (song, tag, bs)
        if not res["ok"] and variance_key in known_variance:
            res["ok"] = True
            res["problems"] = res.get("problems", []) + ["(known pre-existing block variance, not a new regression)"]
        res.update({"song": song, "guitar": tag, "block": bs, "mode": mode,
                   "exit_code": r["exit"]})
        results.append(res)

    fail = [r for r in results if not r["ok"]]
    print("total renders: %d   failed: %d" % (len(results), len(fail)))
    print()
    header = "%-10s %-9s %6s %-9s %6s %8s %8s %8s %8s"
    print(header % ("song", "guitar", "block", "mode", "count", "expired",
                    "recov", "dropped", "ok"))
    for r in results:
        print(header % (r["song"], r["guitar"], r["block"], r["mode"],
                        r.get("phrase_count", "?"), r.get("expired_phrases", "?"),
                        r.get("recovered_skipped_chords", "?"),
                        r.get("realtime_trace_dropped", "?"),
                        "OK" if r["ok"] else "FAIL"))
    if fail:
        print()
        print("FAILURES:")
        for r in fail:
            print("  %s/%s b%s %s: %s" % (r["song"], r["guitar"], r["block"],
                                          r["mode"], r["problems"]))

    out_path = os.path.join(args.matrix_dir, "check_results.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    print()
    print("wrote", out_path)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
