"""Page read/unseal markdown table for a run: dataset facts + per-benchmark counters.

Writes <run_dir>/page_metrics_summary.md. Dataset facts (cluster count, on-disk
page count) come from each benchmark's metadata.txt; read/unseal counts come
from summary.csv.
"""
from __future__ import annotations

from pathlib import Path

from .data import dataset_facts_by_container, read_summary, run_max_page_size


def _ratio(unsealed: str, read: str) -> str:
    try:
        u, r = float(unsealed), float(read)
        return f"{u / r:.2f}x" if r else "--"
    except ValueError:
        return "--"


def write_page_metrics_md(run_dir: Path) -> Path | None:
    rows = read_summary(run_dir / "summary.csv")
    if not rows or "n_page_read" not in rows[0]:
        return None

    out_path = run_dir / "page_metrics_summary.md"
    facts = dataset_facts_by_container(run_dir)

    lines = ["# Page read / unseal comparison", "", f"Run: `{run_dir}`", ""]

    max_page = run_max_page_size(run_dir)
    if max_page:
        lines += [
            f"Max unzipped page size: **{max_page['mib']:g} MiB** ({max_page['bytes']} bytes)",
            "",
        ]

    if facts:
        lines += ["## Dataset facts (fixed per file, not benchmark-dependent)", ""]
        multi_file = len({rf for _, rf in facts}) > 1
        if multi_file:
            lines.append("| container | root_file | clusters | on-disk pages |")
            lines.append("|---|---|---:|---:|")
            for (container, root_file), f in sorted(facts.items()):
                lines.append(f"| {container} | `{root_file}` | {f['clusters']} | {f['pages']} |")
        else:
            lines.append("| container | clusters | on-disk pages |")
            lines.append("|---|---:|---:|")
            for (container, _), f in sorted(facts.items()):
                lines.append(f"| {container} | {f['clusters']} | {f['pages']} |")
        lines.append("")

    lines += ["## Per-benchmark counters", ""]
    lines.append(
        "| # | variant | access | os cache | cluster_cache | pages read | pages unsealed | ratio | clusters loaded |"
    )
    lines.append("|---:|---|---|---|---|---:|---:|---:|---:|")
    for r in rows:
        ratio = _ratio(r["n_page_unsealed"], r["n_page_read"])
        lines.append(
            f"| {r['benchmark_num']} | {r['variant']} | {r['access_pattern']} | "
            f"{r['cache_state']} | {r['cluster_cache']} | {r['n_page_read']} | "
            f"{r['n_page_unsealed']} | {ratio} | {r['n_cluster_loaded']} |"
        )

    out_path.write_text("\n".join(lines) + "\n")
    return out_path
