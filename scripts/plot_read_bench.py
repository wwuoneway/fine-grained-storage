#!/usr/bin/env python3
"""
Plot the read-benchmark results emitted by fgs_read_bench.

fgs_read_bench writes one timestamped run folder per invocation, e.g.:

    output/benchmarks/reading-benchmarks/<timestamp>/
        summary.csv                one row per benchmark (aggregates)
        run_info.json              specs + config
        benchmark_<N>/
            csv/raw.csv            raw: one row per repetition
            runs/run_<M>.txt       per-repetition metrics + details
            metadata.txt / summary.txt / benchmark.log

This script reads those CSVs and writes three PNGs into <run_dir>/plots/:

    latency_by_variant_cache.png   mean latency per benchmark, grouped by
                                   variant and cold/warm cache state
    per_rep_stability.png          wall_s vs repetition, one line per benchmark
                                   (shows run-to-run noise)
    throughput.png                 mean events/s per benchmark

Usage (run from the repo root):
    python3 scripts/plot_read_bench.py [RUN_DIR] [--out OUT_DIR]

    RUN_DIR   a specific run folder; if omitted, the newest timestamped run
              under output/benchmarks/reading-benchmarks/ is used.
    --out     where to write the PNGs; defaults to <RUN_DIR>/plots.

Requires: Python 3, matplotlib. Uses only the stdlib csv module otherwise.
"""

import argparse
import csv
import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless: write files, never open a window
import matplotlib.pyplot as plt

DEFAULT_OUTPUT_BASE = Path("output/benchmarks/reading-benchmarks")


def newest_run_dir(base: Path) -> Path:
    """Return the most recent timestamped run folder under `base`."""
    if not base.is_dir():
        raise SystemExit(f"output base does not exist: {base}\nRun fgs_read_bench first.")
    runs = [p for p in base.iterdir() if p.is_dir() and (p / "summary.csv").exists()]
    if not runs:
        raise SystemExit(
            f"no run folders with summary.csv under {base}\n"
            f"Run fgs_read_bench first, or pass a RUN_DIR explicitly."
        )
    return max(runs, key=lambda p: p.name)  # timestamps sort chronologically


def read_csv(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def load_summary(run_dir: Path) -> list[dict]:
    rows = read_csv(run_dir / "summary.csv")
    for r in rows:
        for k in ("wall_s_mean", "wall_s_min", "latency_us_mean", "latency_us_min",
                  "throughput_evt_s_mean"):
            r[k] = float(r[k])
    return rows


def load_raw(run_dir: Path) -> dict[str, list[tuple[int, float]]]:
    """Map benchmark name -> [(repetition, wall_s), ...] from each benchmark's raw CSV."""
    series: dict[str, list[tuple[int, float]]] = {}
    for csv_path in sorted(run_dir.glob("benchmark_*/csv/raw.csv")):
        rows = read_csv(csv_path)
        if not rows:
            continue
        name = rows[0]["benchmark"]
        series[name] = [(int(r["repetition"]), float(r["wall_s"])) for r in rows]
    return series


def plot_latency_by_variant_cache(summary: list[dict], out: Path) -> None:
    labels = [f"{r['variant']}\n{r['cache_state']}" for r in summary]
    means = [r["latency_us_mean"] for r in summary]
    mins = [r["latency_us_min"] for r in summary]
    # lower whisker = distance from min to mean (best-case is faster)
    yerr_low = [m - lo for m, lo in zip(means, mins)]

    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(max(6, 1.5 * len(labels)), 4.5))
    ax.bar(x, means, yerr=[yerr_low, [0] * len(means)], capsize=4, color="#4c72b0")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_ylabel("latency (µs / event)")
    ax.set_title("Read latency (mean; whisker = best rep)")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out / "latency_by_variant_cache.png", dpi=120)
    plt.close(fig)


def plot_per_rep_stability(series: dict, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for name, points in series.items():
        points.sort()
        reps = [p[0] for p in points]
        wall = [p[1] for p in points]
        ax.plot(reps, wall, marker="o", label=name)
    ax.set_xlabel("repetition")
    ax.set_ylabel("wall time (s)")
    ax.set_title("Per-repetition wall time (run-to-run stability)")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "per_rep_stability.png", dpi=120)
    plt.close(fig)


def plot_throughput(summary: list[dict], out: Path) -> None:
    labels = [f"{r['variant']}\n{r['cache_state']}" for r in summary]
    thr = [r["throughput_evt_s_mean"] for r in summary]
    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(max(6, 1.5 * len(labels)), 4.5))
    ax.bar(x, thr, color="#55a868")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_ylabel("throughput (events / s)")
    ax.set_title("Read throughput (mean)")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out / "throughput.png", dpi=120)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("run_dir", nargs="?", default=None,
                    help="run folder; default = newest under the output base")
    ap.add_argument("--out", default=None, help="output dir for PNGs (default <RUN_DIR>/plots)")
    args = ap.parse_args()

    run_dir = Path(args.run_dir) if args.run_dir else newest_run_dir(DEFAULT_OUTPUT_BASE)
    if not (run_dir / "summary.csv").exists():
        raise SystemExit(
            f"no summary.csv in {run_dir}\n"
            f"Run fgs_read_bench first, or pass the run folder explicitly."
        )

    out = Path(args.out) if args.out else run_dir / "plots"
    os.makedirs(out, exist_ok=True)

    summary = load_summary(run_dir)
    series = load_raw(run_dir)

    plot_latency_by_variant_cache(summary, out)
    plot_per_rep_stability(series, out)
    plot_throughput(summary, out)

    print(f"run   : {run_dir}")
    print(f"plots : {out}")
    for name in ("latency_by_variant_cache.png", "per_rep_stability.png", "throughput.png"):
        print(f"  - {name}")


if __name__ == "__main__":
    main()
