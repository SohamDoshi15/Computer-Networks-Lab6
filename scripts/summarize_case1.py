#!/usr/bin/env python3
"""Summarise Results/Part2_Case1_runs.csv: mean per (fault mode, probability).

Usage: python3 scripts/summarize_case1.py [Results] -> writes Part2_Case1_summary.csv
and prints a Markdown table. Uses only the standard library.
"""
import csv
import statistics
import sys
from collections import OrderedDict

out_dir = sys.argv[1] if len(sys.argv) > 1 else "Results"
rows = list(csv.DictReader(open(f"{out_dir}/Part2_Case1_runs.csv")))
baseline = list(csv.DictReader(open(f"{out_dir}/Part2_Case1_part1_baseline.csv")))
base_goodput = statistics.mean(float(r["goodput_MBps"]) for r in baseline)
base_time = statistics.mean(float(r["completion_s"]) for r in baseline)

groups = OrderedDict()
for r in rows:
    groups.setdefault((r["fault_mode"], r["failure_probability"]), []).append(r)

fields = ["fault_mode", "failure_probability", "runs", "completed_runs", "mean_attempts",
          "expected_attempts", "useful_MB", "transmitted_MB", "redundant_MB", "wasted_MB",
          "overhead", "completion_s", "completion_s_stdev", "goodput_MBps",
          "throughput_MBps", "aggregate_goodput_MBps", "goodput_vs_part1"]
summary = []
for (mode, p), rs in groups.items():
    ok = [r for r in rs if r["status"] == "0"]
    f = lambda k, src=rs: statistics.mean(float(r[k]) for r in src)
    pv = float(p)
    expected = (1 / (1 - pv)) if mode == "connection" and pv < 1 else float("nan")
    summary.append({
        "fault_mode": mode, "failure_probability": p, "runs": len(rs),
        "completed_runs": len(ok),
        "mean_attempts": round(f("attempts"), 2),
        "expected_attempts": round(expected, 2) if expected == expected else "",
        "useful_MB": round(f("useful_bytes", ok) / 1e6, 2) if ok else 0,
        "transmitted_MB": round(f("transmitted_bytes") / 1e6, 2),
        "redundant_MB": round(f("redundant_bytes", ok) / 1e6, 2) if ok else "",
        "wasted_MB": round(f("wasted_bytes", ok) / 1e6, 2) if ok else "",
        "overhead": round(f("overhead", ok), 3) if ok else "",
        "completion_s": round(f("completion_s"), 3),
        "completion_s_stdev": round(statistics.stdev(float(r["completion_s"]) for r in rs), 3)
        if len(rs) > 1 else 0,
        "goodput_MBps": round(f("goodput_MBps", ok), 2) if ok else 0,
        "throughput_MBps": round(f("throughput_MBps"), 2),
        # useful bytes / mean completion time: the rate a user actually sees.
        "aggregate_goodput_MBps": round(f("useful_bytes", ok) / f("completion_s") / 1e6, 2)
        if len(ok) == len(rs) else 0,
        "goodput_vs_part1": round(f("useful_bytes", ok) / f("completion_s") / 1e6
                                  / (statistics.mean(float(r["useful_bytes"]) for r in baseline)
                                     / base_time / 1e6), 3) if len(ok) == len(rs) else 0,
    })

with open(f"{out_dir}/Part2_Case1_summary.csv", "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=fields, lineterminator="\n")
    w.writeheader()
    w.writerows(summary)

print(f"Part 1 baseline (server_multi): {base_time:.3f} s, {base_goodput:.2f} MB/s "
      f"over {len(baseline)} runs\n")
print("| " + " | ".join(fields) + " |")
print("|" + "---|" * len(fields))
for s in summary:
    print("| " + " | ".join(str(s[k]) for k in fields) + " |")
