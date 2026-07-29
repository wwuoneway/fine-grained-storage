"""Load summary.csv rows and the per-metric metadata (axes, defaults, formatting)."""
from __future__ import annotations

import csv
import json
import math
import re
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
    "read_payload_mib",   # bytes pulled from storage
    "n_read",             # read amplification (byte-range reads)
    "read_efficiency",    # payload / (payload + overhead)
    "n_page_read",        # sealed pages fetched from storage
    "n_page_unsealed",    # pages actually decompressed
    "n_cluster_loaded",   # clusters fetched from storage
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
    "read_payload_mib": ("lower = better", "RdYlGn_r"),
    "n_read": ("lower = better", "RdYlGn_r"),
    "read_efficiency": ("higher = better", "RdYlGn"),
    "n_page_read": ("lower = better", "RdYlGn_r"),
    "n_page_unsealed": ("lower = better", "RdYlGn_r"),
    "n_cluster_loaded": ("lower = better", "RdYlGn_r"),
    "locate_ms": ("lower = better", "RdYlGn_r"),
    "load_ms": ("lower = better", "RdYlGn_r"),
    "fill_ms": ("lower = better", "RdYlGn_r"),
}


def read_summary(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def _first_manifest(run_dir: Path) -> dict | None:
    """The writing manifest.json referenced by the run's first benchmark."""
    for meta_path in sorted(run_dir.glob("benchmarks/benchmark_*/metadata.txt")):
        m = re.search(r"^manifest_file\s*:\s*(.+)$", meta_path.read_text(), re.M)
        if not m:
            continue
        manifest_path = Path(m.group(1).strip())
        if not manifest_path.exists():
            continue
        try:
            return json.loads(manifest_path.read_text())
        except json.JSONDecodeError:
            continue
    return None


def run_payload_mib(run_dir: Path) -> float | None:
    """Raw (uncompressed) generated payload per event, in MiB. None if not found
    (older run, or field absent)."""
    manifest = _first_manifest(run_dir)
    return manifest.get("avg_raw_payload_mib") if manifest else None


def run_generation_summary(run_dir: Path) -> dict | None:
    """Events generated + realized particles/event range. None if not found."""
    manifest = _first_manifest(run_dir)
    if manifest is None or "particles_per_event_min" not in manifest:
        return None
    return {
        "num_events": manifest["total_events"],
        "particles_min": manifest["particles_per_event_min"],
        "particles_max": manifest["particles_per_event_max"],
    }


FACTS_LINE = re.compile(r"^\s+(\S+)\s*:\s*clusters=(\d+)\s+pages=(\d+)\s*$")


def dataset_facts_by_container(run_dir: Path) -> dict[tuple[str, str], dict]:
    """(container, root_file) -> {clusters, pages}, read from each benchmark's metadata.txt."""
    facts: dict[tuple[str, str], dict] = {}
    for meta_path in sorted(run_dir.glob("benchmarks/benchmark_*/metadata.txt")):
        text = meta_path.read_text()
        root_file_m = re.search(r"^root_file\s*:\s*(.+)$", text, re.M)
        root_file = root_file_m.group(1).strip() if root_file_m else "?"
        in_facts = False
        for line in text.splitlines():
            if line.startswith("dataset_facts"):
                in_facts = True
                continue
            if not in_facts:
                continue
            m = FACTS_LINE.match(line)
            if not m:
                break
            container, clusters, pages = m.groups()
            facts[(container, root_file)] = {"clusters": int(clusters), "pages": int(pages)}
    return facts


def run_cluster_page_summary(run_dir: Path) -> dict | None:
    """clusters + total on-disk pages, summed across containers of the first
    benchmark's root_file. None if metadata carries no dataset_facts (older run).
    """
    facts = dataset_facts_by_container(run_dir)
    if not facts:
        return None
    first_root_file = next(iter(facts))[1]
    same_file = {k: v for k, v in facts.items() if k[1] == first_root_file}
    clusters = next(iter(same_file.values()))["clusters"]
    total_pages = sum(v["pages"] for v in same_file.values())
    return {"clusters": clusters, "total_pages": total_pages}


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
    if metric in ("read_wall_ms", "unzip_wall_ms", "read_payload_mib",
                  "locate_ms", "load_ms", "fill_ms"):
        return f"{v:.2f}"
    if metric in ("n_page_read", "n_page_unsealed", "n_cluster_loaded"):
        return f"{v:.0f}"
    return f"{v:.1f}"
