"""Stacked-bar wall-time bottleneck breakdown.

Coarse view: I/O / Decompress / Other, decomposing the clean-pass wall. When
the sub-timers are present (locate_ms/load_ms/fill_ms plus wall_instr_ms),
"Other" is split into Decode / Fill / Locate / Loop using the nested model:
load = io + unzip + decode, so decode = load - io - unzip, and
loop = wall_instr - locate - load - fill.

The fine split decomposes wall_instr_ms (the instrumented pass's own wall)
rather than the clean wall_s_mean: the sub-timers are nested inside that wall,
so the loop residual cannot go negative from cross-pass mismatch. For the same
reason its I/O and Decompress segments use read_wall_instr_ms and
unzip_wall_instr_ms, the ROOT counters of that same instrumented execution;
the coarse split keeps the clean-pass counters, matching its clean wall.
"""
import itertools

import matplotlib.pyplot as plt
import matplotlib.ticker

from .data import distinct
from .grid import panel_grid

SEG_COLORS = {
    "I/O": "#4c72b0",
    "Decompress": "#dd8452",
    "Decode": "#55a868",
    "Fill": "#8172b3",
    "Locate": "#937860",
    "Loop": "#8c8c8c",
    "Other": "#8c8c8c",
}


def _seg_names(rows):
    fine = ("locate_ms", "load_ms", "fill_ms", "wall_instr_ms",
            "read_wall_instr_ms", "unzip_wall_instr_ms")
    if all(k in rows[0] for k in fine):
        return ["I/O", "Decompress", "Decode", "Fill", "Locate", "Loop"]
    return ["I/O", "Decompress", "Other"]


def _segments(r, seg_names):
    """One row -> [(segment, ms)]. The coarse split sums to the clean wall; the
    fine split sums to the instrumented pass's wall (wall_instr_ms), the run the
    sub-timers were actually measured on. max(0, ...) only absorbs clock noise.
    """
    if "Other" in seg_names:
        io = float(r["read_wall_ms"])
        unz = float(r["unzip_wall_ms"])
        wall = float(r["wall_s_mean"]) * 1000.0
        return [("I/O", io), ("Decompress", unz), ("Other", max(0.0, wall - io - unz))]
    io = float(r["read_wall_instr_ms"])
    unz = float(r["unzip_wall_instr_ms"])
    wall_instr = float(r["wall_instr_ms"])
    load = float(r["load_ms"])
    locate = float(r["locate_ms"])
    fill = float(r["fill_ms"])
    decode = max(0.0, load - io - unz)
    loop = max(0.0, wall_instr - locate - load - fill)
    return [("I/O", io), ("Decompress", unz), ("Decode", decode),
            ("Fill", fill), ("Locate", locate), ("Loop", loop)]


def _cell_segments(cell, seg_names):
    """Mean per-segment ms across the rows in one (col, facet) cell."""
    out = {name: 0.0 for name in seg_names}
    for r in cell:
        for name, val in _segments(r, seg_names):
            out[name] += val
    return {name: out[name] / len(cell) for name in seg_names}



def _draw_panel(ax, sel, bar_axis, bar_vals, seg_names, y_top):
    """One stacked bar per bar_axis value, drawn into a caller-owned axes."""
    x = range(len(bar_vals))
    seg_vals = {name: [] for name in seg_names}
    for bv in bar_vals:
        cell = [r for r in sel if r[bar_axis] == bv]
        means = _cell_segments(cell, seg_names) if cell else {s: 0.0 for s in seg_names}
        for name in seg_names:
            seg_vals[name].append(means[name])

    bottom = [0.0] * len(bar_vals)
    for name in seg_names:
        # Seconds, to match the units the other figures report.
        vals = [v / 1000.0 for v in seg_vals[name]]
        ax.bar(x, vals, label=name, color=SEG_COLORS[name], bottom=bottom, width=0.75)
        bottom = [b + v for b, v in zip(bottom, vals)]

    for j, total in enumerate(bottom):
        if total > 0:
            ax.text(j, total * 1.03, f"{total:.1f}", ha="center", va="bottom", rotation=90)

    ax.set_ylim(0, y_top)
    ax.set_xticks(list(x))
    ax.set_xticklabels(bar_vals, rotation=45, ha="right")
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_ylabel("time (s)")
    ax.grid(axis="y", alpha=0.3)
    ax.set_axisbelow(True)


def plot_bottleneck_breakdown(rows, bar_axis, panel_axis, facet_axes, plots_dir, num_events,
                              payload_mib=None, cluster_page_summary=None,
                              generation_summary=None, max_page_size=None,
                              container_summary=None):
    """One figure: a stacked-bar breakdown per bar_axis value, panelled by panel_axis.

    Segments are I/O / Decompress / (Other, or the fine split). The Y scale is
    shared across panels so they are directly comparable. Returns the PNG path,
    or None when the counters are missing or inconsistent.
    """
    if not rows or "read_wall_ms" not in rows[0] or "unzip_wall_ms" not in rows[0]:
        return None
    no_counters = [r for r in rows
                   if float(r["read_wall_ms"]) == 0 and float(r["unzip_wall_ms"]) == 0]
    if len(no_counters) == len(rows):
        return None
    if no_counters:
        # Mixed provenance: bars would silently compare different measurement
        # setups (counters on vs off).
        names = sorted({r.get("benchmark", "?") for r in no_counters})
        print("bottleneck: skipping, benchmarks without counters mixed with "
              f"counted ones: {', '.join(names)}")
        return None

    seg_names = _seg_names(rows)
    bar_vals = distinct(rows, bar_axis)

    panel_axes = [panel_axis] + list(facet_axes)
    panel_vals = [distinct(rows, a) for a in panel_axes]
    combos = list(itertools.product(*panel_vals))

    global_max = 0.0
    for r in rows:
        global_max = max(global_max, sum(v for _, v in _segments(r, seg_names)))
    y_top = global_max / 1000.0 * 1.35

    panel_w = max(4.5, 0.44 * len(bar_vals))
    fig, axes = panel_grid(len(combos), panel_w=panel_w, panel_h=3.8, max_cols=2)
    for ax, combo in zip(axes, combos):
        sel = [r for r in rows if all(r[a] == v for a, v in zip(panel_axes, combo))]
        _draw_panel(ax, sel, bar_axis, bar_vals, seg_names, y_top)
        ax.set_title("   ".join(f"{a} = {v}" for a, v in zip(panel_axes, combo)),
                     pad=10)

    subtitle = []
    if generation_summary is not None:
        subtitle.append(
            f"{generation_summary['num_events']} events generated "
            f"({generation_summary['particles_min']}-{generation_summary['particles_max']}"
            " particles/event)")
    if payload_mib is not None:
        subtitle.append(f"{payload_mib:.3f} MiB / event (raw)")
    if cluster_page_summary is not None:
        subtitle.append(f"{cluster_page_summary['clusters']} clusters, "
                        f"{cluster_page_summary['total_pages']} pages on disk")
    if max_page_size is not None:
        subtitle.append(f"{max_page_size['mib']:g} MiB max page size")
    if container_summary is not None:
        subtitle.append(container_summary)

    title = f"Wall-time bottleneck breakdown   [num_events={num_events}]"
    if subtitle:
        title += "\n" + "   ".join(subtitle)
    fig.suptitle(title)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, 0.0),
               ncol=len(seg_names))
    fig.tight_layout(rect=(0, 0.08, 1, 0.94))

    png = plots_dir / "bottleneck.png"
    fig.savefig(png)
    plt.close(fig)
    return png
