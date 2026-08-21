"""Read-cost metrics against scatter distance, one panel per metric on a shared
page, one curve per dataset.

Takes a sweep root holding one directory per dataset, the same shape
event_size.py reads. Sequential access has no jump distance, so its point sits
at distance 0 on the same axis as the scatter sweep (symlog, so 0 is not
squeezed against 1).
"""
from __future__ import annotations

import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker
import matplotlib.colors as mcolors

from .data import (AXIS_SLUG, FIGURE_AXES, METRICS, distinct, fmt_events_per_page,
                   sweep_runs)
from .heatmap import draw_panel
from .linelabels import despine, label_lines
from .pivot import pivot


def _rows(summary: Path):
    with open(summary, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("access_pattern") in ("sequential", "scatter"):
                yield r


def collect(sweep_root: Path) -> dict[tuple, dict]:
    """figure key -> {"curves": {events_per_page -> {distance -> {metric: value}}},
    "reps": set}.

    The figure key is (page_mib, *FIGURE_AXES values), same faceting as
    event_size.py, so rows that differ on a held-fixed axis cannot land on one
    curve unnoticed.
    """
    out: dict[tuple, dict] = {}
    for summary, events_per_page, page_mib in sweep_runs(sweep_root):
        for r in _rows(summary):
            vals = {}
            for metric, spec in METRICS.items():
                try:
                    vals[metric] = float(r[metric]) * spec.scale
                except (KeyError, TypeError, ValueError):
                    continue
            if not vals:
                continue

            distance = 0.0 if r["access_pattern"] == "sequential" else float(r["scatter_distance"])
            key = (page_mib,) + tuple(r.get(a, "") for a in FIGURE_AXES)
            figure = out.setdefault(key, {"curves": {}, "reps": set()})
            points = figure["curves"].setdefault(events_per_page, {})
            if distance in points and points[distance] != vals:
                raise SystemExit(
                    f"two rows differ but share a curve: {key} {events_per_page} events/page "
                    f"at distance {distance}. An axis is missing from FIGURE_AXES."
                )
            points[distance] = vals
            figure["reps"].add(r.get("reps"))
    return out


def _describe(key: dict, figure: dict) -> str:
    """The settings this whole figure holds fixed, for the subtitle."""
    parts = [key["variant"]]
    if len(figure["reps"]) == 1:
        parts.append(f"mean of {next(iter(figure['reps']))} reps")
    if key["cache_state"]:
        parts.append(f"{key['cache_state']} cache")
    if key["cluster_cache"]:
        parts.append(f"cluster cache {key['cluster_cache']}")
    if key["implicit_mt"]:
        parts.append(f"implicit MT {key['implicit_mt']}")
    if key["page_mib"]:
        parts.append(f"{key['page_mib']:g} MiB pages")
    return "   |   ".join(p for p in parts if p)


def _figure_name(key: tuple, every_key: list[tuple], metric_slug: str) -> str:
    """File name carrying the axes that actually separate this figure from the
    others, so a single-variant sweep does not grow pointless suffixes."""
    parts = []
    if key[0]:
        parts.append(f"pg{key[0]:g}mib")
    for i, axis in enumerate(FIGURE_AXES, start=1):
        if not key[i] or (axis != "variant" and len({k[i] for k in every_key}) == 1):
            continue
        parts.append(AXIS_SLUG.get(axis, "{}").format(key[i]))
    parts.append(metric_slug)
    return "read_cost_vs_scatter_distance__" + "__".join(parts) + ".png"


def _panel(ax, curves, metric, cmap):
    """One metric against scatter distance. Returns [(x, y, text, colour)], the
    rightmost point of each drawn line, for direct labeling."""
    steps = sorted(curves)
    # Page granularity is ordinal, so a sequential colormap lets the eye read the
    # order directly instead of decoding an arbitrary qualitative cycle.
    norm = mcolors.LogNorm(vmin=min(steps), vmax=max(steps)) if len(steps) > 1 else None

    plotted = []
    ends = []
    for events_per_page in steps:
        points = curves[events_per_page]
        xs = [x for x in sorted(points) if metric in points[x]]
        if not xs:
            continue
        ys = [points[x][metric] for x in xs]
        plotted += ys
        colour = cmap(norm(events_per_page)) if norm else cmap(0.5)
        ax.plot(xs, ys, marker="o", markersize=4,
                markeredgecolor="white", markeredgewidth=0.5, linewidth=1.2,
                color=colour, zorder=3)
        ends.append((xs[-1], ys[-1], f"{fmt_events_per_page(events_per_page)} ev/page", colour))
    if not ends:
        return None

    spec = METRICS[metric]
    ax.set_xscale("symlog", linthresh=1)
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    if spec.log and min(plotted) < max(plotted):
        ax.set_yscale("log")
        ax.set_ylabel(f"{spec.label} (log scale)")
        # Decades alone leave two or three labels on a ratio axis.
        ax.yaxis.set_major_locator(
            matplotlib.ticker.LogLocator(base=10.0, subs=(1.0, 2.0, 3.0, 4.0, 5.0, 7.0),
                                         numticks=15))
        ax.get_yaxis().set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.get_yaxis().set_minor_formatter(matplotlib.ticker.NullFormatter())
    else:
        ax.set_ylabel(spec.label)
        if min(plotted) == max(plotted):
            # A metric that never moves has no scale to speak of.
            ax.set_ylim(min(plotted) * 0.5, max(plotted) * 1.5)
        ax.yaxis.set_major_locator(matplotlib.ticker.MaxNLocator(nbins=9, min_n_ticks=5))
    despine(ax)
    return ends


def plot_scatter_curves(sweep_root: Path,
                        metrics: list[str],
                        out_dir: Path | None = None) -> list[Path]:
    """One figure per variant / page size / cache setting / metric."""
    out_dir = out_dir or sweep_root
    cmap = plt.get_cmap("viridis")
    drawn = [m for m in metrics if m in METRICS]
    figures = collect(sweep_root)
    written = []
    width, height = 7.0, 5.0

    for key, figure in sorted(figures.items()):
        curves = figure["curves"]
        if not drawn or not curves:
            continue
        named = dict(zip(["page_mib"] + FIGURE_AXES, key))
        ticks = sorted({x for points in curves.values() for x in points})

        for metric in drawn:
            spec = METRICS[metric]
            fig, ax = plt.subplots(figsize=(width, height))
            ends = _panel(ax, curves, metric, cmap)
            if not ends:
                plt.close(fig)
                continue
            ax.set_xticks(ticks)
            ax.tick_params(labelbottom=True)
            ax.set_xlabel("scatter distance (events, symlog scale)")
            # Reserve room to the right of the data for line-end labels: values
            # converge near distance 0, so the spread is at the far end.
            ax.set_xlim(right=ticks[-1] * 2.6)

            offset_in = 0.25
            fig.suptitle(f"{spec.name.capitalize()} against scatter distance",
                        y=1.0 - offset_in / height)
            offset_in += 0.30

            conditions = _describe(named, figure)
            if conditions:
                fig.text(0.5, 1.0 - offset_in / height, conditions, ha="center", va="top",
                         fontsize=8, color="0.35")
                offset_in += 0.22
            offset_in += 0.12

            fig.subplots_adjust(left=0.14, right=0.95, top=1.0 - offset_in / height,
                                bottom=1.0 / height)
            label_lines(ax, ends, label_x=ticks[-1] * 1.5, ha="left")

            subdir = out_dir / "read_cost" / "scatter_distance"
            subdir.mkdir(parents=True, exist_ok=True)
            path = subdir / _figure_name(key, list(figures), spec.slug)
            if path in written:
                raise SystemExit(f"two figures would be written to {path.name}: {key}")
            fig.savefig(path)
            plt.close(fig)
            written.append(path)
    return written


def _heatmap_rows(sweep_root: Path) -> dict[tuple, list[dict]]:
    """figure key -> CSV rows with "events_per_page" injected, sorted so distinct()
    below yields ascending events-per-page and scatter-distance order."""
    out: dict[tuple, list[dict]] = {}
    for summary, events_per_page, page_mib in sweep_runs(sweep_root):
        for r in _rows(summary):
            key = (page_mib,) + tuple(r.get(a, "") for a in FIGURE_AXES)
            row = dict(r)
            row["events_per_page"] = f"{events_per_page:g}"
            out.setdefault(key, []).append(row)
    for rows in out.values():
        rows.sort(key=lambda r: (float(r["events_per_page"]), float(r["scatter_distance"])))
    return out


def _heatmap_figure_name(key: tuple, every_key: list[tuple], metric_slug: str) -> str:
    parts = []
    if key[0]:
        parts.append(f"pg{key[0]:g}mib")
    for i, axis in enumerate(FIGURE_AXES, start=1):
        if not key[i] or (axis != "variant" and len({k[i] for k in every_key}) == 1):
            continue
        parts.append(AXIS_SLUG.get(axis, "{}").format(key[i]))
    parts.append(metric_slug)
    return "heatmap_scatter_vs_events_per_page__" + "__".join(parts) + ".png"


def plot_scatter_heatmap(sweep_root: Path,
                         metrics: list[str],
                         out_dir: Path | None = None) -> list[Path]:
    """One grid per metric per facet: rows are events per page, columns are
    scatter distance, each cell coloured and annotated with its value. The
    rectangle equivalent of plot_scatter_curves, over the same rows."""
    out_dir = out_dir or sweep_root
    figures = _heatmap_rows(sweep_root)
    written = []

    for key, rows in sorted(figures.items()):
        named = dict(zip(["page_mib"] + FIGURE_AXES, key))
        row_vals = distinct(rows, "events_per_page")
        col_vals = distinct(rows, "scatter_distance")
        row_labels = [fmt_events_per_page(float(v)) for v in row_vals]
        col_labels = ["0 (seq)" if v == "0" else v for v in col_vals]
        figure_stub = {"reps": {r.get("reps") for r in rows}}
        conditions = _describe(named, figure_stub)

        for metric in metrics:
            if metric not in METRICS:
                continue
            spec = METRICS[metric]
            grid = pivot(rows, metric, "events_per_page", "scatter_distance", row_vals, col_vals)
            if spec.scale != 1.0:
                grid = [[v * spec.scale for v in gr] for gr in grid]
            flat = [x for gr in grid for x in gr if not math.isnan(x)]
            if not flat:
                continue

            cmap = plt.get_cmap(spec.cmap).copy()
            # NaN cells as light gray: transparent-over-white on RdYlGn reads as
            # a mid-scale value.
            cmap.set_bad("#e8e8e8")
            fig, ax = plt.subplots(
                figsize=(1.1 * len(col_vals) + 2.0, 0.6 * len(row_vals) + 2.2))
            im = draw_panel(ax, grid, metric, row_labels, col_labels, cmap, "",
                            min(flat), max(flat))
            ax.set_ylabel("events per page")
            ax.set_xlabel("scatter distance (events)")
            fig.colorbar(im, ax=ax, shrink=0.85, label=spec.label)

            title = f"{spec.label}  ({spec.direction})"
            if conditions:
                title += "\n" + conditions
            fig.suptitle(title)
            fig.tight_layout(rect=(0, 0, 1, 0.90 if conditions else 0.94))

            subdir = out_dir / "heatmap"
            subdir.mkdir(parents=True, exist_ok=True)
            path = subdir / _heatmap_figure_name(key, list(figures), spec.slug)
            if path in written:
                raise SystemExit(f"two figures would be written to {path.name}: {key}")
            fig.savefig(path)
            plt.close(fig)
            written.append(path)
    return written
