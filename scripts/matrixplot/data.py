"""Load summary.csv rows and the per-metric metadata (axes, defaults, formatting)."""
from __future__ import annotations

import csv
import math
from pathlib import Path

# The sweep axes that appear as summary.csv columns.
AXES = ["variant", "access_pattern", "cache_state", "cluster_cache", "implicit_mt"]

DEFAULT_METRICS = [
    "latency_us_mean",
    "throughput_evt_s_mean",
    "wall_s_mean",
    # ROOT RNTuple counters (bottleneck attribution), if present:
    "read_wall_ms",       # time in storage I/O
    "unzip_wall_ms",      # time decompressing
    "read_payload_mb",    # bytes pulled from storage
    "n_read",             # read amplification (byte-range reads)
    "read_efficiency",    # payload / (payload + overhead)
    # "Other"-segment breakdown (instrumented pass):
    "locate_ms",          # row-range lookup
    "load_ms",            # LoadEntry decode
    "fill_ms",            # per-event vector alloc + copy
]

# Optimisation direction per metric, used for the title and the colormap.
DIRECTION = {
    "latency_us_mean": ("lower = better", "RdYlGn_r"),
    "wall_s_mean": ("lower = better", "RdYlGn_r"),
    "throughput_evt_s_mean": ("higher = better", "RdYlGn"),
    "read_wall_ms": ("lower = better", "RdYlGn_r"),
    "unzip_wall_ms": ("lower = better", "RdYlGn_r"),
    "read_payload_mb": ("lower = better", "RdYlGn_r"),
    "n_read": ("lower = better", "RdYlGn_r"),
    "read_efficiency": ("higher = better", "RdYlGn"),
    "locate_ms": ("lower = better", "RdYlGn_r"),
    "load_ms": ("lower = better", "RdYlGn_r"),
    "fill_ms": ("lower = better", "RdYlGn_r"),
}


def read_summary(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def distinct(rows: list[dict], col: str) -> list[str]:
    """Distinct values of a column, in first-seen order."""
    out: list[str] = []
    for r in rows:
        if r[col] not in out:
            out.append(r[col])
    return out


def fmt(metric: str, v: float) -> str:
    if math.isnan(v):
        return "--"
    if metric == "wall_s_mean":
        return f"{v:.4f}"
    if metric == "read_efficiency":
        return f"{v:.3f}"
    if metric in ("throughput_evt_s_mean", "n_read"):
        return f"{v:.0f}"
    if metric in ("read_wall_ms", "unzip_wall_ms", "read_payload_mb",
                  "locate_ms", "load_ms", "fill_ms"):
        return f"{v:.2f}"
    return f"{v:.1f}"
