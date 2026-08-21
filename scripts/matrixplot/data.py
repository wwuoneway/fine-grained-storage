"""Load summary.csv rows and the per-metric metadata (axes, labels, formatting)."""
from __future__ import annotations

import csv
import functools
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# The sweep axes that appear as summary.csv columns.
AXES = ["variant", "access_pattern", "cache_state", "cluster_cache", "implicit_mt"]

# Everything except access_pattern: these pick the figure, not the curve, in the
# cross-dataset comparison figures (event_size.py, scatter_curves.py).
FIGURE_AXES = ["variant", "cache_state", "cluster_cache", "implicit_mt"]

# How each axis reads in a file name. Bare on/off would say nothing.
AXIS_SLUG = {"cluster_cache": "cc-{}", "implicit_mt": "imt-{}"}


@dataclass(frozen=True)
class Metric:
    label: str    # axis and colourbar text, carrying the unit
    scale: float  # multiply the raw column by this before plotting
    fmt: str      # format spec for cell annotations
    better: str   # "lower" or "higher"
    slug: str     # output filename stem
    log: bool = False  # ratios spanning orders of magnitude need a log axis

    @property
    def name(self) -> str:
        """Label without its unit, for figure titles."""
        return self.label.split(" (")[0]

    @property
    def cmap(self) -> str:
        return "RdYlGn" if self.better == "higher" else "RdYlGn_r"

    @property
    def direction(self) -> str:
        return f"{self.better} = better"


METRICS = {
    "wall_s_mean":      Metric("wall time (s)", 1.0, ".2f", "lower", "wall_time"),
    "unzip_wall_ms":    Metric("decompression time (s)", 1e-3, ".2f", "lower", "decompression"),
    "read_payload_mib": Metric("bytes read (MiB)", 1.0, ".1f", "lower", "read_payload"),
    "n_page_read":      Metric("pages read", 1.0, ".0f", "lower", "pages_read"),
    # The amplifications are ratios of like units, so they carry no unit of their
    # own; the axis says so rather than leaving a reader to guess at bytes.
    "decomp_amplification":    Metric("decompressed / wanted bytes (x)", 1.0, ".2f", "lower",
                                      "decomp_amplification", log=True),
    "unzip_gb_s":              Metric("decompression throughput (GB/s)", 1.0, ".2f", "higher",
                                      "decomp_throughput"),
    "page_read_amplification": Metric("pages fetched / pages in file (x)", 1.0, ".2f", "lower",
                                      "page_read_amplification", log=True),
    "page_unseal_amplification": Metric("pages unzipped / pages in file (x)", 1.0, ".2f",
                                        "lower", "page_unseal_amplification", log=True),
}

# The read-cost ratios, the default panels of the event-size figure. Pages
# fetched and pages unzipped are both kept: one is I/O, the other is CPU,
# and they move independently.
RATIO_METRICS = ["decomp_amplification",
                 "page_read_amplification", "page_unseal_amplification"]


def read_summary(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    # Fold the distance into access_pattern rather than adding an axis: every
    # plot groups by access_pattern, so scatter distances separate everywhere at
    # once, and non-scatter rows keep a single value instead of a facet of NAs.
    for r in rows:
        if r.get("access_pattern") == "scatter":
            r["access_pattern"] = f"scatter-{r.get('scatter_distance', '?')}"
    return rows


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


def run_column_pages(run_dir: Path) -> int | None:
    """Pages in the widest column of the files this run read (a particle
    parameter, where all but a handful of a file's pages live), from the
    columns.json fgs_inspect_columns writes. None if that pass has not run."""
    path = run_dir / "columns.json"
    if not path.exists():
        return None
    try:
        by_file = json.loads(path.read_text())
    except json.JSONDecodeError:
        return None
    pages = [n for containers in by_file.values()
             for columns in containers.values() for n in columns.values()]
    return max(pages) if pages else None


def run_events_per_page(run_dir: Path) -> float | None:
    """Events covered by one page of the widest column: the dataset's event
    count over that column's page count. None if either is unavailable."""
    manifest = _first_manifest(run_dir)
    pages = run_column_pages(run_dir)
    if manifest is None or not pages or "total_events" not in manifest:
        return None
    return manifest["total_events"] / pages


def fmt_events_per_page(v: float) -> str:
    """Ticks and labels: whole events once a page holds ten of them, two
    decimals below that, where a page covers a fraction of an event."""
    return f"{v:,.0f}" if v >= 10 else f"{v:.2f}"


@functools.lru_cache(maxsize=None)
def sweep_runs(sweep_root: Path) -> tuple[tuple[Path, float, float], ...]:
    """(summary.csv, events_per_page, page_mib) per run under the sweep root.

    A run with no manifest or no columns.json cannot be placed on the x axis.
    Dropping it is reported, so a cleaned dataset cannot pass for a smaller study.
    """
    runs, skipped = [], []
    for summary in sorted(sweep_root.glob("**/summary.csv")):
        run_dir = summary.parent
        events_per_page = run_events_per_page(run_dir)
        if events_per_page is None:
            skipped.append(run_dir)
            continue
        page = run_max_page_size(run_dir)
        runs.append((summary, events_per_page, page["mib"] if page else 0.0))
    if skipped:
        print(f"sweep_runs: skipping {len(skipped)} run(s) with no readable manifest or no "
              f"columns.json (run fgs_inspect_columns over them):", file=sys.stderr)
        for run_dir in skipped:
            print(f"  {run_dir}", file=sys.stderr)
    return tuple(runs)


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


def container_summary(rows: list[dict]) -> str | None:
    """Which containers were read, e.g. "1 container (position_container)".
    None when the rows predate the column, or disagree."""
    values = {r.get("containers") for r in rows}
    if len(values) != 1 or not (names := values.pop()):
        return None
    read = names.split("|")
    return f"{len(read)} container{'s' if len(read) > 1 else ''} ({' + '.join(read)})"


MAX_PAGE_LINE = re.compile(r"^max_page_size\s*:\s*(\d+) bytes", re.M)


def run_max_page_size(run_dir: Path) -> dict | None:
    """MaxUnzippedPageSize the benchmarked files were written with, as
    {bytes, mib}. None for runs whose metadata predates the field."""
    for meta_path in sorted(run_dir.glob("benchmarks/benchmark_*/metadata.txt")):
        m = MAX_PAGE_LINE.search(meta_path.read_text())
        if m:
            b = int(m.group(1))
            return {"bytes": b, "mib": b / (1 << 20)}
    return None


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
    spec = METRICS[metric].fmt if metric in METRICS else ".1f"
    return f"{v:{spec}}"
