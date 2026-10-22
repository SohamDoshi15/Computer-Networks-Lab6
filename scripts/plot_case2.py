#!/usr/bin/env python3
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

out_dir = sys.argv[1] if len(sys.argv) > 1 else "Results"
path = os.path.join(out_dir, "Part2_Case2_runs.csv")
if not os.path.exists(path):
    raise SystemExit(f"error: {path} not found; run run_case2_experiments.sh first")

rows = [r for r in csv.DictReader(open(path, newline="", encoding="utf-8"))
        if r["fault_mode"] == "connection" and r["status"] == "0"]
if not rows:
    raise SystemExit(f"error: {path} has no completed connection-mode measurements")

data = defaultdict(list)
for row in rows:
    data[float(row["failure_probability"])].append(row)
ps = sorted(data)

fig, ax = plt.subplots(figsize=(8, 5))
means = [np.mean([float(r["goodput_MBps"]) for r in data[p]]) for p in ps]
stdevs = [np.std([float(r["goodput_MBps"]) for r in data[p]]) for p in ps]
ax.errorbar(ps, means, yerr=stdevs, color="#1f77b4", marker="o", linewidth=2, capsize=4)
ax.set_title("Part 2 Case 2: Goodput vs Failure Probability")
ax.set_xlabel("Failure probability")
ax.set_ylabel("Useful goodput (MB/s)")
ax.grid(True, linestyle="--", alpha=0.5)
fig.tight_layout()
target = os.path.join(out_dir, "Part2_Case2_throughput.png")
fig.savefig(target, dpi=200)
plt.close(fig)

fig, ax = plt.subplots(figsize=(8, 5))
redundant = [np.mean([float(r["redundant_bytes"]) / max(1.0, float(r["useful_bytes"])) * 100
                      for r in data[p]]) for p in ps]
ax.plot(ps, redundant, color="#7570b3", marker="s", linewidth=2)
ax.set_title("Part 2 Case 2: Redundant Data vs Failure Probability")
ax.set_xlabel("Failure probability")
ax.set_ylabel("Redundant bytes (% of useful bytes)")
ax.grid(True, linestyle="--", alpha=0.5)
fig.tight_layout()
target2 = os.path.join(out_dir, "Part2_Case2_overhead.png")
fig.savefig(target2, dpi=200)
print(f"Generated {target} and {target2}")
