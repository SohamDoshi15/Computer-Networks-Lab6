#!/usr/bin/env python3
"""Graphs for Part 2 Case 1 from Results/Part2_Case1_runs.csv (needs matplotlib).

Usage: python3 scripts/plot_case1.py [Results] [topics_root/topic]

Writes Part2_Case1_throughput.png, Part2_Case1_overhead.png and
Part2_Case1_bytes.png. Error bars span the minimum and maximum of the runs;
dashed lines are the analytical models in docs/FAULT_INJECTION_CASE1.md.
"""
import csv
import math
import os
import statistics
import sys
from collections import OrderedDict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

out_dir = sys.argv[1] if len(sys.argv) > 1 else "Results"
topic_dir = sys.argv[2] if len(sys.argv) > 2 else None

rows = list(csv.DictReader(open(os.path.join(out_dir, "Part2_Case1_runs.csv"))))
baseline = list(csv.DictReader(open(os.path.join(out_dir, "Part2_Case1_part1_baseline.csv"))))
base_goodput = statistics.mean(float(r["goodput_MBps"]) for r in baseline)


def series(mode, key, only_ok=True):
    groups = OrderedDict()
    for r in rows:
        if r["fault_mode"] != mode or (only_ok and r["status"] != "0"):
            continue
        groups.setdefault(float(r["failure_probability"]), []).append(float(r[key]))
    xs = list(groups)
    means = [statistics.mean(v) for v in groups.values()]
    lo = [m - min(v) for m, v in zip(means, groups.values())]
    hi = [max(v) - m for m, v in zip(means, groups.values())]
    return xs, means, [lo, hi]


useful = statistics.mean(float(r["useful_bytes"]) for r in rows if r["status"] == "0")
data_frames = None
if topic_dir:
    sizes = [os.path.getsize(os.path.join(topic_dir, f)) for f in os.listdir(topic_dir)
             if os.path.isfile(os.path.join(topic_dir, f))]
    data_frames = sum(max(1, math.ceil(s / 65536)) for s in sizes if s > 0)

# 1. Goodput and raw throughput vs p. The lines are aggregate rates (mean bytes /
#    mean completion time); a mean of per-run ratios would overweight lucky runs.
#    Faint dots are individual runs.
def aggregate(mode):
    groups = OrderedDict()
    for r in rows:
        if r["fault_mode"] == mode:
            groups.setdefault(float(r["failure_probability"]), []).append(r)
    xs, good, raw, pts = [], [], [], []
    for p, rs in groups.items():
        t = statistics.mean(float(r["completion_s"]) for r in rs)
        ok = [r for r in rs if r["status"] == "0"]
        xs.append(p)
        good.append(useful / t / 1e6 if len(ok) == len(rs) else float("nan"))
        raw.append(statistics.mean(float(r["transmitted_bytes"]) for r in rs) / t / 1e6)
        pts += [(p, float(r["goodput_MBps"])) for r in ok]
    return xs, good, raw, pts


fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
for ax, mode, label in ((axes[0], "connection", "per-connection failure probability p"),
                        (axes[1], "chunk", "per-DATA-frame failure probability q")):
    x, good, raw, pts = aggregate(mode)
    ax.scatter([a for a, _ in pts], [b for _, b in pts], s=8, alpha=0.25, color="C0",
               label="goodput of single runs")
    ax.plot(x, good, marker="o", color="C0", label="goodput = useful / mean completion time")
    ax.plot(x, raw, marker="s", color="C1", label="raw throughput = transmitted / time")
    ax.axhline(base_goodput, color="grey", linestyle=":", label="Part 1 multi-threaded")
    if mode == "connection":
        xs = [i / 100 for i in range(0, 91)]
        ax.plot(xs, [base_goodput * (1 - p) / (1 - p / 2) for p in xs], "k--", linewidth=1,
                label="model: Part 1 x (1-p)/(1-p/2)")
    ax.set_xlabel(label)
    ax.set_ylabel("MB/s")
    ax.set_title(f"Case 1, {mode} mode")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=7)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case1_throughput.png"), dpi=150)

# 2. Overhead ((transmitted - useful) / useful) and attempts vs p and q.
fig, axes = plt.subplots(1, 3, figsize=(15, 4.2))
x, m, err = series("connection", "overhead")
axes[0].errorbar(x, m, yerr=err, marker="o", capsize=3, label="measured")
xs = [i / 100 for i in range(0, 91)]
axes[0].plot(xs, [p / (2 * (1 - p)) for p in xs], "k--", linewidth=1,
             label="model: p / (2(1-p))")
axes[0].set_xlabel("per-connection failure probability p")
axes[0].set_ylabel("overhead = (transmitted - useful) / useful")
axes[0].set_title("Case 1 overhead, connection mode")
axes[0].grid(alpha=0.3)
axes[0].legend(fontsize=8)

x, m, err = series("connection", "attempts", only_ok=False)
axes[1].errorbar(x, m, yerr=err, marker="o", capsize=3, label="measured")
axes[1].plot(xs, [1 / (1 - p) for p in xs], "k--", linewidth=1, label="model: 1 / (1-p)")
axes[1].set_xlabel("per-connection failure probability p")
axes[1].set_ylabel("attempts to finish")
axes[1].set_title("Case 1 attempts, connection mode")
axes[1].grid(alpha=0.3)
axes[1].legend(fontsize=8)

xc, mc, errc = series("chunk", "attempts", only_ok=False)
axes[2].errorbar(xc, mc, yerr=errc, marker="s", capsize=3,
                 label="measured (capped at CN_MAX_ATTEMPTS)")
if data_frames:
    qs = [i / 10000 for i in range(0, 51)]
    axes[2].plot(qs, [(1 - q) ** -data_frames for q in qs], "k--", linewidth=1,
                 label=f"model: (1-q)^-n, n = {data_frames} DATA frames")
axes[2].set_yscale("log")
axes[2].set_ylim(0.8, 2e4)
axes[2].set_xlabel("per-DATA-frame failure probability q")
axes[2].set_ylabel("attempts (log scale)")
axes[2].set_title("Case 1 attempts, chunk mode")
axes[2].grid(alpha=0.3, which="both")
axes[2].legend(fontsize=8)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case1_overhead.png"), dpi=150)

# 3. Useful vs redundant vs protocol bytes (stacked) per p, connection mode.
fig, ax = plt.subplots(figsize=(7, 4.2))
x, red, _ = series("connection", "redundant_bytes")
_, trans, _ = series("connection", "transmitted_bytes")
_, rec, _ = series("connection", "data_bytes_received")
useful_mb = [useful / 1e6] * len(x)
red_mb = [r / 1e6 for r in red]
proto_mb = [(t - d) / 1e6 for t, d in zip(trans, rec)]
labels = [f"{p:g}" for p in x]
ax.bar(labels, useful_mb, label="useful file bytes")
ax.bar(labels, red_mb, bottom=useful_mb, label="redundant file bytes (re-sent)")
ax.bar(labels, proto_mb, bottom=[u + r for u, r in zip(useful_mb, red_mb)],
       label="headers, manifests, partial frames")
ax.set_xlabel("per-connection failure probability p")
ax.set_ylabel("MB received by the client (mean)")
ax.set_title("Case 1: where the transmitted bytes go")
ax.grid(alpha=0.3, axis="y")
ax.legend(fontsize=8)
fig.tight_layout()
fig.savefig(os.path.join(out_dir, "Part2_Case1_bytes.png"), dpi=150)
print("wrote Part2_Case1_throughput.png, Part2_Case1_overhead.png, Part2_Case1_bytes.png")
