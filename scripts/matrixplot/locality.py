"""Access-locality cost curves: one curve per page size, one figure per metric.

Takes several run dirs because a run dir holds exactly one max page size.
Sequential and random carry no jump distance, so each gets a narrow axis of its
own at either end of the sweeps.
"""
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker

from .data import METRICS, container_summary, run_generation_summary, run_max_page_size

MARKERS = ["o", "s", "^", "D", "v", "P", "X"]

# (access_pattern value, x label) for the swept knob.
KNOBS = [("scatter", "scatter distance (events)")]


def _palette():
    return plt.rcParams["axes.prop_cycle"].by_key()["color"]


def _series(run_dirs):
    """page_mib -> {pattern -> {x -> {metric -> value}}}, merged across run dirs."""
    out: dict[float, dict] = {}
    for d in run_dirs:
        summary = d / "summary.csv"
        if not summary.exists():
            continue
        page = run_max_page_size(d)
        if page is None:
            continue
        bucket = out.setdefault(page["mib"], {})
        with open(summary, newline="") as f:
            for r in csv.DictReader(f):
                vals = {m: float(r[m]) * spec.scale
                        for m, spec in METRICS.items() if r.get(m) not in (None, "")}
                if not vals:
                    continue
                pattern = r["access_pattern"]
                key = r.get("scatter_distance")
                x = float(key) if pattern == "scatter" else 0.0
                bucket.setdefault(pattern, {})[x] = vals
    return out


def _conditions(run_dirs):
    """The settings held fixed across the sweep, for the subtitle. Read from the
    data so the caption cannot drift from what was actually run."""
    first = next((d for d in run_dirs if (d / "summary.csv").exists()), None)
    if first is None:
        return ""
    with open(first / "summary.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return ""

    def fixed(col):
        vals = {r[col] for r in rows if col in r}
        return vals.pop() if len(vals) == 1 else None

    parts = []
    gen = run_generation_summary(first)
    if gen and gen["particles_min"] == gen["particles_max"]:
        parts.append(f"{gen['num_events']} events x {gen['particles_min']:,} particles")
    elif fixed("num_events"):
        parts.append(f"{fixed('num_events')} events")
    for col, fmt in (("variant", "{}"),
                     ("cache_state", "{} cache"),
                     ("cluster_cache", "cluster cache {}"),
                     ("reps", "mean of {} reps")):
        v = fixed(col)
        if v:
            parts.append(fmt.format(v))
    containers = container_summary(rows)
    if containers:
        parts.append(containers)
    return "   |   ".join(parts).replace(",", " ")


def _style_axes(ax):
    ax.grid(True, axis="y", zorder=0)
    ax.set_axisbelow(True)


def _plot_sweep(ax, series, xlabel, pattern, metric):
    """The swept knob: one curve per page size. Returns False if no data."""
    drew = False
    seen_x = set()
    palette = _palette()
    for i, (page_mib, patterns) in enumerate(sorted(series.items())):
        # Wrap rather than truncate: more page sizes than colours must not
        # silently drop a curve.
        points = [(x, v) for x, v in sorted(patterns.get(pattern, {}).items()) if metric in v]
        seen_x.update(x for x, _ in points)
        if not points:
            continue
        ax.plot([x for x, _ in points], [v[metric] for _, v in points],
                marker=MARKERS[i % len(MARKERS)], markersize=4,
                markeredgecolor="white", markeredgewidth=0.6, linewidth=1.4,
                color=palette[i % len(palette)], zorder=3)
        drew = True
    if not drew:
        return False
    # Linear: a swept axis is whatever values the config listed, and they need
    # not be geometric (distances 2,4,8,10 are not).
    ticks = sorted(seen_x)
    ax.set_xticks(ticks)
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.get_xaxis().set_major_formatter(
        matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.set_xlabel(xlabel)
    _style_axes(ax)
    return True


def _plot_baseline(ax, series, pattern, metric, tick_label):
    """Sequential and random have no jump distance, so each gets its own axis."""
    palette = _palette()
    ends = []
    for i, (page_mib, patterns) in enumerate(sorted(series.items())):
        vals = patterns.get(pattern, {}).get(0.0)
        if not vals or metric not in vals:
            continue
        colour = palette[i % len(palette)]
        ax.plot([0], [vals[metric]], marker=MARKERS[i % len(MARKERS)], markersize=5,
                markeredgecolor="white", markeredgewidth=0.6, color=colour,
                linestyle="none", zorder=3)
        ends.append((0.0, vals[metric], f"{page_mib:g} MiB", colour))
    ax.set_xlim(-0.5, 0.5)
    ax.set_xticks([0])
    ax.set_xticklabels([tick_label])
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    _style_axes(ax)
    return ends


def _label_gap(ax, points=11.0):
    """`points` of vertical clearance expressed in data units. A fraction of the
    y span would collapse exactly when the series converge and need it most."""
    axes_pt = ax.get_window_extent().height * 72.0 / ax.get_figure().dpi
    span = ax.get_ylim()[1] - ax.get_ylim()[0]
    return points * span / axes_pt


def _stack(ends, gap):
    """[(y_marker, y_label, text, colour)], pushed up until none collide."""
    placed, out = [], []
    for _x, y0, text, colour in sorted(ends, key=lambda e: e[1]):
        y = y0
        while any(abs(y - p) < gap for p in placed):
            y += gap
        placed.append(y)
        out.append((y0, y, text, colour))
    return out


def _label_ends(ax, ends, x_right):
    """Write each series name past the right spine, nudged apart where they collide."""
    if not ends:
        return
    gap = _label_gap(ax)
    stacked = _stack(ends, gap)
    # Growing the axes rescales every position, so make room first and restack
    # against the new span rather than drawing into a scale that then changes.
    top = max(y for _, y, _, _ in stacked)
    if top > ax.get_ylim()[1]:
        ax.set_ylim(top=top + gap)
        gap = _label_gap(ax)
        stacked = _stack(ends, gap)

    for y0, y, text, colour in stacked:
        # A leader line, because converging series get pushed far enough from
        # their marker that the label alone would point at the wrong value.
        arrow = None
        if abs(y - y0) > gap / 4:
            arrow = dict(arrowstyle="-", color=colour, lw=0.5,
                         shrinkA=0, shrinkB=1, alpha=0.7)
        ax.annotate(text, xy=(x_right, y0), xytext=(x_right + 0.35, y),
                    textcoords="data", va="center", ha="left",
                    color=colour, annotation_clip=False, arrowprops=arrow)


def _one_metric(series, metric, out_path, conditions=""):
    """seq | scatter | rand, sharing y."""
    spec = METRICS[metric]
    fig, axes = plt.subplots(
        1, len(KNOBS) + 2, sharey=True, squeeze=False,
        gridspec_kw={"width_ratios": [0.13] + [1.0] * len(KNOBS) + [0.13], "wspace": 0.07},
    )
    ax_seq, *ax_sweeps, ax_rand = axes[0]

    _plot_baseline(ax_seq, series, "sequential", metric, "seq")
    ax_seq.set_ylabel(spec.label)

    any_data = False
    for ax, (pattern, xlabel) in zip(ax_sweeps, KNOBS):
        ok = _plot_sweep(ax, series, xlabel, pattern, metric)
        any_data = any_data or ok
        if not ok:
            ax.set_visible(False)
            continue
        ax.set_title(f"{pattern} access", pad=8)
    if not any_data:
        plt.close(fig)
        return None

    ends = _plot_baseline(ax_rand, series, "random", metric, "random")

    # One explicit limit for the shared axis. Left to autoscale, the axis styled
    # last would set the top and clip whichever curve peaks somewhere else.
    values = [v[metric]
              for patterns in series.values()
              for points in patterns.values()
              for v in points.values() if metric in v]
    if not values:
        plt.close(fig)
        return None
    ax_seq.set_ylim(0, max(values) * 1.05)

    fig.suptitle(f"{spec.name.capitalize()} against access locality", y=0.98)
    if conditions:
        fig.text(0.5, 0.90, conditions, ha="center", va="top", fontsize=8, color="0.35")

    # Label placement measures the axes in points, so fix the geometry first.
    fig.subplots_adjust(left=0.075, right=0.87, top=0.79, bottom=0.14)
    _label_ends(ax_rand, ends, 0.5)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path)
    plt.close(fig)
    return out_path


def plot_locality_curves(run_dirs: list[Path], out_dir: Path) -> list[Path]:
    """One figure per metric. Returns the paths written, empty if no run had data."""
    series = _series(run_dirs)
    if not series:
        return []
    conditions = _conditions(run_dirs)
    written = []
    for metric, spec in METRICS.items():
        png = _one_metric(series, metric, out_dir / f"{spec.slug}.png", conditions)
        if png is not None:
            written.append(png)
    return written
