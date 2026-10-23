#!/usr/bin/env python3
"""Graphs for Part 2 Case 2 Enhanced: Parallel TCP connections and Range Recovery.

Usage: python3 scripts/plot_case2_enhanced.py [Results]

Generates:
  - Part2_Case2_Enhanced_throughput.png
  - Part2_Case2_Enhanced_overhead.png
  - Part2_Case2_Enhanced_speedup.png
  - Case_Comparison_Throughput.png (if baseline CSVs exist)
"""

import csv
import os
import sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

out_dir = sys.argv[1] if len(sys.argv) > 1 else "Results"
csv_path = os.path.join(out_dir, "Part2_Case2_Enhanced_runs.csv")

if not os.path.exists(csv_path):
    raise SystemExit(
        f"error: {csv_path} not found; run "
        "scripts/run_case2_enhanced_experiments.sh with a real dataset first"
    )

rows = list(csv.DictReader(open(csv_path)))
if not rows:
    raise SystemExit(f"error: {csv_path} contains no measurement rows")

# Group by (N, p)
data = defaultdict(lambda: defaultdict(list))
for r in rows:
    n = int(r["parallel_N"])
    p = float(r["failure_probability"])
    mbps = float(r["throughput_mbps"])
    red = float(r["redundant_bytes"])
    useful = float(r["useful_bytes"])
    ovh = float(r["overhead_ratio"])
    data[n][p].append({
        "mbps": mbps,
        "red": red,
        "useful": useful,
        "ovh": ovh
    })

# 1. Throughput vs Failure Probability for different N
fig, ax = plt.subplots(figsize=(8, 5))
colors = {1: "#7f7f7f", 2: "#1f77b4", 4: "#2ca02c", 8: "#d62728"}
markers = {1: "o", 2: "s", 4: "^", 8: "D"}

for n in sorted(data.keys()):
    ps = sorted(data[n].keys())
    means = [np.mean([x["mbps"] for x in data[n][p]]) for p in ps]
    std = [np.std([x["mbps"] for x in data[n][p]]) for p in ps]
    ax.errorbar(ps, means, yerr=std, label=f"N = {n} connections",
                color=colors.get(n, "black"), marker=markers.get(n, "o"),
                capsize=4, linewidth=2)

ax.set_title("Part 2 Case 2 Enhanced: Throughput vs Failure Probability (p)", fontsize=13)
ax.set_xlabel("Failure Probability (p)", fontsize=11)
ax.set_ylabel("Aggregate Throughput (MB/s)", fontsize=11)
ax.grid(True, linestyle="--", alpha=0.6)
ax.legend(loc="upper right", frameon=True)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case2_Enhanced_throughput.png"), dpi=200)
plt.close(fig)
print(f"Generated {os.path.join(out_dir, 'Part2_Case2_Enhanced_throughput.png')}")

# 2. Overhead and Retransmission Ratio vs Failure Probability
fig, ax = plt.subplots(figsize=(8, 5))
for n in sorted(data.keys()):
    ps = sorted(data[n].keys())
    ratios = [np.mean([x["red"] / max(1.0, x["useful"]) for x in data[n][p]]) * 100.0 for p in ps]
    ax.plot(ps, ratios, label=f"N = {n} connections",
            color=colors.get(n, "black"), marker=markers.get(n, "o"),
            linewidth=2)

ax.set_title("Part 2 Case 2 Enhanced: Retransmission Overhead vs Failure Probability", fontsize=13)
ax.set_xlabel("Failure Probability (p)", fontsize=11)
ax.set_ylabel("Redundant Bytes (% of Useful Data)", fontsize=11)
ax.grid(True, linestyle="--", alpha=0.6)
ax.legend(loc="upper left", frameon=True)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case2_Enhanced_overhead.png"), dpi=200)
plt.close(fig)
print(f"Generated {os.path.join(out_dir, 'Part2_Case2_Enhanced_overhead.png')}")

# 3. Speedup Scaling vs Connection Count N
fig, ax = plt.subplots(figsize=(7, 5))
ns = sorted(data.keys())
clean_rates = [np.mean([x["mbps"] for x in data[n][0.0]]) for n in ns]
base_rate = clean_rates[0] if clean_rates and clean_rates[0] > 0 else 1.0
speedup_clean = [r / base_rate for r in clean_rates]

# Also under the smallest measured non-zero fault probability common to all N.
common_probabilities = set(data[ns[0]])
for n in ns[1:]:
    common_probabilities &= set(data[n])
fault_probabilities = sorted(p for p in common_probabilities if p > 0.0)
fault_p = fault_probabilities[0] if fault_probabilities else 0.0
rates_fault = [np.mean([x["mbps"] for x in data[n][fault_p]]) for n in ns]
base_fault = rates_fault[0] if rates_fault and rates_fault[0] > 0 else 1.0
speedup_fault = [r / base_fault for r in rates_fault]

ax.plot(ns, speedup_clean, marker="o", color="#2ca02c", linewidth=2.5, label="Clean (p = 0.0)")
ax.plot(ns, speedup_fault, marker="s", color="#d62728", linewidth=2.5,
        label=f"Fault Injected (p = {fault_p:g})")
ax.plot(ns, ns, linestyle=":", color="gray", label="Ideal Linear Speedup")

ax.set_title("Enhanced Case 2: Parallel Speedup vs Connection Count (N)", fontsize=13)
ax.set_xlabel("Number of Parallel TCP Connections (N)", fontsize=11)
ax.set_ylabel("Speedup (Normalized to N = 1)", fontsize=11)
ax.set_xticks(ns)
ax.grid(True, linestyle="--", alpha=0.6)
ax.legend(loc="upper left", frameon=True)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case2_Enhanced_speedup.png"), dpi=200)
plt.close(fig)
print(f"Generated {os.path.join(out_dir, 'Part2_Case2_Enhanced_speedup.png')}")

# 4. Multi-Case Comparative Summary
fig, ax = plt.subplots(figsize=(9, 5.5))
case1_csv = os.path.join(out_dir, "Part2_Case1_runs.csv")
case1_pts = {}
if os.path.exists(case1_csv):
    for r in csv.DictReader(open(case1_csv)):
        if r.get("fault_mode") == "connection" and r.get("status") == "0":
            p = float(r["failure_probability"])
            mbps = float(r.get("goodput_MBps", r.get("throughput_MBps", 0)))
            case1_pts.setdefault(p, []).append(mbps)

# Case 1 curve
if case1_pts:
    c1_ps = sorted(case1_pts)
    c1_means = [np.mean(case1_pts[p]) for p in c1_ps]
    ax.plot(c1_ps, c1_means, marker="x", color="#d62728", linewidth=2, label="Case 1 (Restart from scratch)")

# Case 2 curve from its own measured CSV, when available.
case2_csv = os.path.join(out_dir, "Part2_Case2_runs.csv")
case2_pts = {}
if os.path.exists(case2_csv):
    for r in csv.DictReader(open(case2_csv)):
        if r.get("fault_mode") == "connection" and r.get("status") == "0":
            p = float(r["failure_probability"])
            case2_pts.setdefault(p, []).append(float(r["goodput_MBps"]))
if case2_pts:
    c2_ps = sorted(case2_pts)
    c2_means = [np.mean(case2_pts[p]) for p in c2_ps]
    ax.plot(c2_ps, c2_means, marker="s", color="#1f77b4", linewidth=2,
            label="Case 2 (Checkpoint Resume)")

# Case 2 Enhanced curve (N=4 parallel + range resume)
enhanced_n = 4 if 4 in data else max(data)
c2e_ps = sorted(data[enhanced_n])
c2e_means = [np.mean([x["mbps"] for x in data[enhanced_n][p]]) for p in c2e_ps]
ax.plot(c2e_ps, c2e_means, marker="^", color="#2ca02c", linewidth=2.5,
        label=f"Case 2 Enhanced (Parallel N={enhanced_n} + Range Resume)")

ax.set_title("Performance Comparison: Part 2 Case 1 vs Case 2 vs Enhanced Case 2", fontsize=13)
ax.set_xlabel("Failure Probability (p)", fontsize=11)
ax.set_ylabel("Goodput / Throughput (MB/s)", fontsize=11)
ax.grid(True, linestyle="--", alpha=0.6)
ax.legend(loc="upper right", frameon=True)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Case_Comparison_Throughput.png"), dpi=200)
plt.close(fig)
print(f"Generated {os.path.join(out_dir, 'Case_Comparison_Throughput.png')}")
