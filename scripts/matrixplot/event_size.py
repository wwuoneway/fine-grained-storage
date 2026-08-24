"""Read-cost metrics against events per page, one panel per metric on a shared page.

Takes a sweep root holding one directory per dataset. Event size is what varies
between datasets and never within one, so each dataset contributes one x
position: how many events one page of a particle-parameter column covers, which
falls as events grow. Particles per event stays on the fact table below the axis.

Only the access pattern separates curves within a figure. Every other axis
(variant, page size, caches) separates figures instead, so a page never carries
more curves than can be told apart, and no two rows can land on one curve.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker
import matplotlib.colors as mcolors

from .data import (
    AXIS_SLUG,
    FIGURE_AXES,
    METRICS,
    fmt_events_per_page,
    read_summary,
    run_cluster_page_summary,
    run_generation_summary,
    sweep_runs,
)
from .linelabels import despine, label_lines


def collect(sweep_root: Path) -> dict[tuple, dict]:
    """figure key -> {"curves": {pattern -> {events_per_page -> {metric: value}}},
    "events": set, "reps": set}.

    The figure key is (page_mib, *FIGURE_AXES values). A run is any directory
    holding a summary.csv, at any depth, so the dataset nesting is not baked in.
    """
    out: dict[tuple, dict] = {}
    for summary, events_per_page, page_mib in sweep_runs(sweep_root):
        for r in read_summary(summary):
            vals = {}
            for metric, spec in METRICS.items():
                try:
                    vals[metric] = float(r[metric]) * spec.scale
                except (KeyError, TypeError, ValueError):
                    continue
            if not vals:
                continue

            key = (page_mib,) + tuple(r.get(a, "") for a in FIGURE_AXES)
            figure = out.setdefault(key, {"curves": {}, "events": set(), "reps": set()})
            points = figure["curves"].setdefault(r["access_pattern"], {})
            if events_per_page in points and points[events_per_page] != vals:
                raise SystemExit(
                    f"two rows differ but share a curve: {key} {r['access_pattern']} "
                    f"at {events_per_page} events/page. An axis is missing from FIGURE_AXES."
                )
            points[events_per_page] = vals
            figure["events"].add(r.get("num_events"))
            figure["reps"].add(r.get("reps"))
    return out


def dataset_facts(sweep_root: Path) -> dict[float, dict[float, dict]]:
    """page_mib -> {events_per_page -> what that x position was measured on}."""
    out: dict[float, dict[float, dict]] = {}
    for summary, events_per_page, page_mib in sweep_runs(sweep_root):
        run_dir = summary.parent
        gen = run_generation_summary(run_dir)
        shape = run_cluster_page_summary(run_dir)
        if gen is None or shape is None:
            continue
        read = {r["num_events"] for r in read_summary(summary)}
        lo, hi = gen["particles_min"], gen["particles_max"]
        out.setdefault(page_mib, {})[events_per_page] = {
            "events": next(iter(read)) if len(read) == 1 else "-",
            "particles": f"{lo}" if lo == hi else f"{lo}-{hi}",
            "pages": shape["total_pages"],
            "clusters": shape["clusters"],
        }
    return out


# Row order of the small table under the x axis.
FACT_ROWS = ["events", "particles", "pages", "clusters"]


def _pattern_order(pattern: str) -> tuple[int, int]:
    """Increasing disorder, so colours and legend follow the locality axis
    rather than the alphabet, which would put scatter-16 before scatter-4."""
    if pattern == "sequential":
        return (0, 0)
    if pattern.startswith("scatter-"):
        try:
            return (1, int(pattern.split("-", 1)[1]))
        except ValueError:
            return (1, 0)
    return (2, 0)


def _abbrev(pattern: str) -> str:
    """Short form for direct line labels, where "sequential" or "scatter-256"
    would crowd the reserved margin."""
    if pattern == "sequential":
        return "seq"
    if pattern == "random":
        return "rand"
    if pattern.startswith("scatter-"):
        return "sc-" + pattern.split("-", 1)[1]
    return pattern


def _fact_table(fig, ax, facts, xlabel):
    """What each x position was measured on, one column per dataset."""
    box = ax.get_position()
    to_figure = fig.transFigure.inverted()
    row_height = 0.16 / fig.get_figheight()

    top = box.y0 - 0.30 / fig.get_figheight()
    for i, key in enumerate(FACT_ROWS):
        y = top - i * row_height
        fig.text(box.x0 - 0.008, y, key, ha="right", va="center", fontsize=6.5, color="0.45")
        for x, fact in facts.items():
            fx, _ = to_figure.transform(ax.transData.transform((x, ax.get_ylim()[0])))
            fig.text(fx, y, str(fact[key]), ha="center", va="center", fontsize=6.5, color="0.45")

    fig.text(box.x0 + box.width / 2, top - len(FACT_ROWS) * row_height, xlabel,
             ha="center", va="center")


def _describe(key: dict, figure: dict) -> str:
    """The settings this whole figure holds fixed, for the subtitle."""
    parts = [key["variant"]]
    for field, text in (("events", "{} events"), ("reps", "mean of {} reps")):
        if len(figure[field]) == 1:
            parts.append(text.format(next(iter(figure[field]))))
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
        # Variant always names the file: splitting on it is the point.
        if not key[i] or (axis != "variant" and len({k[i] for k in every_key}) == 1):
            continue
        parts.append(AXIS_SLUG.get(axis, "{}").format(key[i]))
    parts.append(metric_slug)
    return "read_cost_vs_events_per_page__" + "__".join(parts) + ".png"


def _panel(ax, curves, metric, cmap):
    """One metric against events/page. Returns [(x, y, pattern, colour)],
    the rightmost point of each drawn line, for direct labeling."""
    patterns = sorted(curves, key=_pattern_order)
    distances = [int(p.split("-")[1]) for p in patterns if p.startswith("scatter-")]
    # Scatter distance is ordinal, so a sequential colormap lets the eye read
    # the order directly instead of decoding an arbitrary qualitative cycle.
    norm = mcolors.LogNorm(vmin=min(distances), vmax=max(distances)) if distances else None

    plotted = []
    ends = []
    for pattern in patterns:
        points = curves[pattern]
        xs = [x for x in sorted(points) if metric in points[x]]
        if not xs:
            continue
        ys = [points[x][metric] for x in xs]
        plotted += ys
        if pattern == "sequential":
            colour = "0.15"
            ax.plot(xs, ys, linestyle="--", color=colour, linewidth=1.6, marker="o",
                    markersize=4, markeredgecolor="white", markeredgewidth=0.5, zorder=4)
        elif pattern == "random":
            colour = "tab:red"
            ax.plot(xs, ys, color=colour, linewidth=1.8, marker="o", markersize=4,
                    markeredgecolor="white", markeredgewidth=0.5, zorder=4)
        else:
            dist = int(pattern.split("-")[1])
            colour = cmap(norm(dist))
            ax.plot(xs, ys, color=colour, linewidth=1.3, marker="o", markersize=3.4,
                    markeredgecolor="white", markeredgewidth=0.4, zorder=3)
        ends.append((xs[-1], ys[-1], _abbrev(pattern), colour))
    if not ends:
        return None

    spec = METRICS[metric]
    ax.set_xscale("log")
    ax.get_xaxis().set_major_formatter(
        matplotlib.ticker.FuncFormatter(lambda v, _: fmt_events_per_page(v)))
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


def plot_event_size_curves(sweep_root: Path,
                           metrics: list[str],
                           out_dir: Path | None = None) -> list[Path]:
    """One figure per variant / page size / cache setting / metric."""
    out_dir = out_dir or sweep_root
    cmap = plt.get_cmap("viridis")
    drawn = [m for m in metrics if m in METRICS]
    facts_by_page = dataset_facts(sweep_root)
    figures = collect(sweep_root)
    written = []
    width, height = 7.0, 5.5

    for key, figure in sorted(figures.items()):
        curves = figure["curves"]
        if not drawn or not curves:
            continue
        named = dict(zip(["page_mib"] + FIGURE_AXES, key))
        ticks = sorted({x for points in curves.values() for x in points})
        # Only what this figure plots: a column with no point above it would
        # describe data from another page.
        facts = {x: f for x, f in facts_by_page.get(key[0], {}).items() if x in ticks}

        for metric in drawn:
            spec = METRICS[metric]
            fig, ax = plt.subplots(figsize=(width, height))
            ends = _panel(ax, curves, metric, cmap)
            if not ends:
                plt.close(fig)
                continue
            ax.set_xticks(ticks)
            ax.tick_params(labelbottom=True)
            # Reserve log-space right of the data for line-end labels, so they
            # sit inside the axes instead of running off the figure.
            ax.set_xlim(ticks[0] / 1.3, ticks[-1] * 3.2)

            # Title and conditions are laid out top-down in inches so their size
            # does not depend on the figure's height.
            offset_in = 0.25
            fig.suptitle(f"{spec.name.capitalize()} against events per page",
                        y=1.0 - offset_in / height)
            offset_in += 0.30

            conditions = _describe(named, figure)
            if conditions:
                fig.text(0.5, 1.0 - offset_in / height, conditions, ha="center", va="top",
                         fontsize=8, color="0.35")
                offset_in += 0.22
            offset_in += 0.12

            fig.subplots_adjust(left=0.14, right=0.95, top=1.0 - offset_in / height,
                                bottom=1.6 / height)
            label_lines(ax, ends, label_x=ticks[-1] * 1.7, ha="left")
            _fact_table(fig, ax, facts,
                        "events per page of a particle-parameter column (log scale)")

            subdir = out_dir / "read_cost" / "events_per_page"
            subdir.mkdir(parents=True, exist_ok=True)
            path = subdir / _figure_name(key, list(figures), spec.slug)
            if path in written:
                raise SystemExit(f"two figures would be written to {path.name}: {key}")
            fig.savefig(path)
            plt.close(fig)
            written.append(path)
    return written
