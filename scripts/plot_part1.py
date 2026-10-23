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
path = os.path.join(out_dir, "Part1_runs.csv")
if not os.path.exists(path):
    raise SystemExit(f"error: {path} not found; run run_part1_experiments.sh first")

data = defaultdict(lambda: defaultdict(list))
for row in csv.DictReader(open(path, newline="", encoding="utf-8")):
    data[row["server_mode"]][int(row["clients"])].append(float(row["aggregate_throughput_MBps"]))
if not data:
    raise SystemExit(f"error: {path} contains no measurement rows")

fig, ax = plt.subplots(figsize=(8, 5))
for mode, color, marker in (("single", "#d95f02", "o"), ("multi", "#1b9e77", "s")):
    counts = sorted(data[mode])
    means = [np.mean(data[mode][n]) for n in counts]
    stdevs = [np.std(data[mode][n]) for n in counts]
    ax.errorbar(counts, means, yerr=stdevs, marker=marker, color=color,
                linewidth=2, capsize=4, label=f"{mode.title()}-threaded server")
ax.set_title("Part 1: Aggregate Throughput vs Concurrent Clients")
ax.set_xlabel("Concurrent clients")
ax.set_ylabel("Aggregate useful throughput (MB/s)")
ax.set_xticks(sorted({n for per_mode in data.values() for n in per_mode}))
ax.grid(True, linestyle="--", alpha=0.5)
ax.legend()
fig.tight_layout()
target = os.path.join(out_dir, "Part1_throughput.png")
fig.savefig(target, dpi=200)
print(f"Generated {target}")
