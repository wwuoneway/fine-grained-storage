"""Step-to-step growth across the dataset steps, every access pattern in one grid
per metric.

Takes the same figures event_size.collect() already builds and looks at
consecutive events/page steps within each pattern's curve: how much a metric
changed, in percent, going from one dataset to the one whose pages cover more
events, which is the one with smaller events.
"""
from __future__ import annotations

import math
from pathlib import Path

import matplotlib.pyplot as plt

from .data import AXIS_SLUG, FIGURE_AXES, METRICS, fmt_events_per_page
from .event_size import _pattern_order, collect


def _growth_grid(curves: dict, metric: str) -> tuple[list[list[float]], list[str], list[str]]:
    """(grid, row_patterns, col_labels) for one metric. grid[i][j] is the
    percent change of `metric` from step j to step j+1, for pattern row_patterns[i]."""
    patterns = sorted(curves, key=_pattern_order)
    steps = sorted({x for points in curves.values() for x in points})
    col_labels = [f"{fmt_events_per_page(lo)}->{fmt_events_per_page(hi)}"
                  for lo, hi in zip(steps, steps[1:])]

    grid = []
    for pattern in patterns:
        points = curves[pattern]
        row = []
        for lo, hi in zip(steps, steps[1:]):
            before = points.get(lo, {}).get(metric)
            after = points.get(hi, {}).get(metric)
            if before is None or after is None or before == 0:
                row.append(math.nan)
            else:
                row.append((after - before) / before * 100.0)
        grid.append(row)
    return grid, patterns, col_labels


def _draw_grid(ax, grid, row_patterns, col_labels, cmap, vmin, vmax):
    im = ax.imshow(grid, aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax)
    ax.set_xticks(range(len(col_labels)), col_labels, rotation=20, ha="right", fontsize=8)
    ax.set_yticks(range(len(row_patterns)), row_patterns, fontsize=8)
    for i in range(len(row_patterns)):
        for j in range(len(col_labels)):
            v = grid[i][j]
            label = "--" if math.isnan(v) else f"{v:+.0f}%"
            ax.text(j, i, label, ha="center", va="center", color="black", fontsize=7.5)
    return im


def _describe(key: dict) -> str:
    """The settings this whole figure holds fixed, for the subtitle."""
    parts = [key["variant"]]
    if key["cache_state"]:
        parts.append(f"{key['cache_state']} cache")
    if key["cluster_cache"]:
        parts.append(f"cluster cache {key['cluster_cache']}")
    if key["implicit_mt"]:
        parts.append(f"implicit MT {key['implicit_mt']}")
    if key["page_mib"]:
        parts.append(f"{key['page_mib']:g} MiB pages")
    return "   |   ".join(p for p in parts if p)


def _figure_name(key: tuple, every_key: list[tuple]) -> str:
    parts = []
    if key[0]:
        parts.append(f"pg{key[0]:g}mib")
    for i, axis in enumerate(FIGURE_AXES, start=1):
        if not key[i] or (axis != "variant" and len({k[i] for k in every_key}) == 1):
            continue
        parts.append(AXIS_SLUG.get(axis, "{}").format(key[i]))
    return "growth_vs_previous_step" + ("__" + "__".join(parts) if parts else "") + ".png"


def plot_growth_curves(sweep_root: Path,
                       metrics: list[str],
                       out_dir: Path | None = None) -> list[Path]:
    """One PNG per variant / page size / cache setting, one panel per metric,
    each panel a pattern x step-transition grid of percent change."""
    out_dir = out_dir or sweep_root
    drawn = [m for m in metrics if m in METRICS]
    figures = collect(sweep_root)
    written = []

    for key, figure in sorted(figures.items()):
        curves = figure["curves"]
        if not drawn or not curves:
            continue

        panels_data = []
        for metric in drawn:
            grid, row_patterns, col_labels = _growth_grid(curves, metric)
            if len(col_labels) == 0:
                continue
            flat = [v for row in grid for v in row if not math.isnan(v)]
            if not flat:
                continue
            panels_data.append((metric, grid, row_patterns, col_labels, flat))
        if not panels_data:
            continue

        named = dict(zip(["page_mib"] + FIGURE_AXES, key))
        n = len(panels_data)
        n_rows = len(panels_data[0][2])
        n_cols = len(panels_data[0][3])
        panel_h = 0.32 * n_rows + 1.1
        fig, axes = plt.subplots(n, 1, figsize=(0.9 * n_cols + 3.0, panel_h * n),
                                 squeeze=False)

        for (metric, grid, row_patterns, col_labels, flat), ax in zip(panels_data, axes[:, 0]):
            spec = METRICS[metric]
            cmap = plt.get_cmap(spec.cmap).copy()
            cmap.set_bad("#e8e8e8")
            vmax = max(abs(v) for v in flat) or 1.0
            im = _draw_grid(ax, grid, row_patterns, col_labels, cmap, -vmax, vmax)
            ax.set_title(f"{spec.label}  ({spec.direction})", fontsize=9)
            fig.colorbar(im, ax=ax, shrink=0.85, label="% change vs. previous step")

        fig.suptitle("Growth vs. previous events-per-page step, by access pattern")
        conditions = _describe(named)
        if conditions:
            fig.text(0.5, 0.965, conditions, ha="center", va="top", fontsize=8, color="0.35")
        fig.tight_layout(rect=(0, 0, 1, 0.94 if conditions else 0.96))

        subdir = out_dir / "growth"
        subdir.mkdir(parents=True, exist_ok=True)
        path = subdir / _figure_name(key, list(figures))
        if path in written:
            raise SystemExit(f"two figures would be written to {path.name}: {key}")
        fig.savefig(path)
        plt.close(fig)
        written.append(path)
    return written
