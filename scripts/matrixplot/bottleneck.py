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
import math

import matplotlib.pyplot as plt

from .data import distinct

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


def _bottleneck_one_variant(variant_rows, variant_name, col_axis, facet_axes,
                            plots_dir, num_events, global_max, seg_names):
    """Single stacked-bar bottleneck chart for one variant.  Returns the PNG path."""
    col_vals = distinct(variant_rows, col_axis)
    facet_vals = [distinct(variant_rows, a) for a in facet_axes]
    combos = list(itertools.product(*facet_vals)) if facet_axes else [()]

    n = len(combos)
    ncols = min(n, 3) if n > 1 else 1
    nrows_fig = math.ceil(n / ncols)

    n_seg = len(seg_names)
    _ax_h_in = 5.5
    _rows_frac = 0.14 * n_seg            # table height as a fraction of axes height
    _hspace = _rows_frac + 0.45          # leave room for the hanging table + next title
    _table_in = (_rows_frac + 0.08) * _ax_h_in
    _panel_title_in = 1.3
    _fig_title_in = 0.9
    _title_in = _panel_title_in + _fig_title_in
    _subplot_in = _ax_h_in * (nrows_fig + (nrows_fig - 1) * _hspace)
    fig_h = _subplot_in + _table_in + _title_in
    top_frac = 1.0 - _title_in / fig_h
    bot_frac = _table_in / fig_h
    _suptitle_y = 1.0 - 0.15 / fig_h
    _legend_y   = 1.0 - 0.50 / fig_h

    panel_w = max(9, 2.8 * len(col_vals))
    fig, axs = plt.subplots(nrows_fig, ncols,
                            figsize=(panel_w * ncols, fig_h),
                            squeeze=False)
    fig.subplots_adjust(wspace=0.4, hspace=_hspace, top=top_frac, bottom=bot_frac)

    y_top = global_max * 1.15

    for pidx, combo in enumerate(combos):
        sel = [r for r in variant_rows
               if all(r[a] == v for a, v in zip(facet_axes, combo))]
        ax = axs[pidx // ncols][pidx % ncols]
        x = range(len(col_vals))

        seg_vals = {name: [] for name in seg_names}
        has_data = []
        for cv in col_vals:
            cell = [r for r in sel if r[col_axis] == cv]
            has_data.append(bool(cell))
            means = _cell_segments(cell, seg_names) if cell else {n_: 0.0 for n_ in seg_names}
            for name in seg_names:
                seg_vals[name].append(means[name])

        bottom = [0.0] * len(col_vals)
        for name in seg_names:
            vals = seg_vals[name]
            ax.bar(x, vals, label=name, color=SEG_COLORS[name], bottom=bottom)
            bottom = [b + v for b, v in zip(bottom, vals)]
        totals = bottom

        for j, total in enumerate(totals):
            if total > 0:
                ax.text(j, total * 1.02, f"{total:.1f} ms",
                        ha="center", va="bottom", fontsize=13, fontweight="bold")

        ax.set_ylim(0, y_top)
        ax.set_xticks(list(x))
        ax.set_xticklabels(col_vals, rotation=0, ha="center", fontsize=13)
        ax.set_ylabel("time (ms)", fontsize=13)
        ax.tick_params(axis="y", labelsize=12)
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{v:.1f}"))
        ax.grid(axis="y", alpha=0.3)

        pairs = [f"{a} = {v}" for a, v in zip(facet_axes, combo)]
        lines = ["   ".join(pairs[i : i + 2]) for i in range(0, len(pairs), 2)]
        ax.set_title("\n".join(lines) if lines else "(all)",
                     fontsize=13, linespacing=1.5, pad=26)

        # "--" for columns with no rows, so absence is not shown as 0.00 ms.
        cell_text, row_colors = [], []
        for name in seg_names:
            row = []
            for j, val in enumerate(seg_vals[name]):
                if not has_data[j]:
                    row.append("--")
                    continue
                pct = val / totals[j] * 100 if totals[j] > 0 else 0
                row.append(f"{val:.2f} ms  ({pct:.0f}%)")
            cell_text.append(row)
            row_colors.append([SEG_COLORS[name]] * len(col_vals))

        tbl = ax.table(
            cellText=cell_text,
            cellColours=row_colors,
            rowLabels=seg_names,
            cellLoc="center",
            loc="bottom",
            bbox=[0, -(0.08 + _rows_frac), 1, _rows_frac],
        )
        tbl.auto_set_font_size(False)
        tbl.set_fontsize(11)
        for (r, c), cell in tbl.get_celld().items():
            cell.set_edgecolor("#dddddd")

    for pidx in range(n, nrows_fig * ncols):
        axs[pidx // ncols][pidx % ncols].axis("off")

    handles, labels = axs[0][0].get_legend_handles_labels()
    fig.suptitle(
        f"Wall-time bottleneck breakdown  ·  {num_events} events  ·  {variant_name}  ·  x = {col_axis}",
        fontsize=17, fontweight="bold", y=_suptitle_y,
    )
    fig.legend(handles, labels,
               loc="upper center", bbox_to_anchor=(0.5, _legend_y),
               ncol=n_seg, fontsize=13,
               framealpha=0.9, edgecolor="#cccccc")

    slug = variant_name.replace("/", "-").replace(" ", "_")
    png = plots_dir / f"bottleneck_{slug}.png"
    fig.savefig(png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return png


def plot_bottleneck_breakdown(rows, col_axis, row_axis, facet_axes, plots_dir, num_events):
    """One stacked-bar breakdown plot per row_axis value (variant).

    Each plot shows a single variant; x-axis = col_axis; one stacked bar per
    col value; segments = I/O / Decompress / (Other, or the fine split). The
    global Y scale is shared across all plots so they are directly comparable.
    """
    if not rows or "read_wall_ms" not in rows[0] or "unzip_wall_ms" not in rows[0]:
        return None
    if all(float(r["read_wall_ms"]) == 0 and float(r["unzip_wall_ms"]) == 0 for r in rows):
        return None

    seg_names = _seg_names(rows)
    row_vals = distinct(rows, row_axis)

    # Global Y max across ALL variants so the plots share one scale.
    global_max = 0.0
    for r in rows:
        global_max = max(global_max, sum(v for _, v in _segments(r, seg_names)))

    pngs = []
    for rv in row_vals:
        variant_rows = [r for r in rows if r[row_axis] == rv]
        png = _bottleneck_one_variant(
            variant_rows, rv, col_axis, facet_axes, plots_dir, num_events, global_max, seg_names
        )
        pngs.append(png)
    return pngs
