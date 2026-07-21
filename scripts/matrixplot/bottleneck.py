"""Stacked-bar wall-time bottleneck breakdown (I/O vs decompress vs other)."""
import itertools
import math

import matplotlib.pyplot as plt

from .data import distinct


def _bottleneck_one_variant(variant_rows, variant_name, col_axis, facet_axes,
                            plots_dir, num_events, global_max):
    """Single stacked-bar bottleneck chart for one variant.  Returns the PNG path."""
    col_vals = distinct(variant_rows, col_axis)
    facet_vals = [distinct(variant_rows, a) for a in facet_axes]
    combos = list(itertools.product(*facet_vals)) if facet_axes else [()]

    n = len(combos)
    ncols = min(n, 3) if n > 1 else 1
    nrows_fig = math.ceil(n / ncols)

    _hspace = 0.8
    _ax_h_in = 5.5
    _table_in = 0.50 * _ax_h_in
    _panel_title_in = 1.3
    _fig_title_in = 0.9
    _title_in = _panel_title_in + _fig_title_in
    _subplot_in = _ax_h_in * (nrows_fig + (nrows_fig - 1) * _hspace)
    fig_h = _subplot_in + _table_in + _title_in
    top_frac = 1.0 - _title_in / fig_h
    bot_frac = _table_in / fig_h
    _suptitle_y = 1.0 - 0.15 / fig_h
    _legend_y   = 1.0 - 0.50 / fig_h

    seg_colors = {
        "I/O": "#4c72b0",
        "Decompress": "#dd8452",
        "Other": "#8c8c8c",
    }

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

        io_vals, unz_vals, oth_vals = [], [], []
        for cv in col_vals:
            cell = [r for r in sel if r[col_axis] == cv]
            if cell:
                io_ms   = sum(float(r["read_wall_ms"]) for r in cell) / len(cell)
                unz_ms  = sum(float(r["unzip_wall_ms"]) for r in cell) / len(cell)
                wall_ms = sum(float(r["wall_s_mean"]) * 1000.0 for r in cell) / len(cell)
                oth = max(0.0, wall_ms - io_ms - unz_ms)
            else:
                io_ms = unz_ms = oth = 0.0
            io_vals.append(io_ms); unz_vals.append(unz_ms)
            oth_vals.append(oth)

        b_unz = io_vals
        b_oth = [a + b for a, b in zip(io_vals, unz_vals)]

        ax.bar(x, io_vals,  label="I/O",        color=seg_colors["I/O"])
        ax.bar(x, unz_vals, label="Decompress", color=seg_colors["Decompress"], bottom=b_unz)
        ax.bar(x, oth_vals, label="Other",      color=seg_colors["Other"],      bottom=b_oth)

        for j, (io, unz, oth) in enumerate(zip(io_vals, unz_vals, oth_vals)):
            total = io + unz + oth
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

        seg_arrays = [io_vals, unz_vals, oth_vals]
        totals = [sum(s[j] for s in seg_arrays) for j in range(len(col_vals))]
        cell_text, row_colors = [], []
        for seg_name, seg_arr in zip(seg_colors.keys(), seg_arrays):
            row = []
            for j, val in enumerate(seg_arr):
                pct = val / totals[j] * 100 if totals[j] > 0 else 0
                row.append(f"{val:.2f} ms  ({pct:.0f}%)")
            cell_text.append(row)
            row_colors.append([seg_colors[seg_name]] * len(col_vals))

        tbl = ax.table(
            cellText=cell_text,
            cellColours=row_colors,
            cellLoc="center",
            loc="bottom",
            bbox=[0, -0.50, 1, 0.42],
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
               ncol=len(seg_colors), fontsize=13,
               framealpha=0.9, edgecolor="#cccccc")

    slug = variant_name.replace("/", "-").replace(" ", "_")
    png = plots_dir / f"bottleneck_{slug}.png"
    fig.savefig(png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return png


def plot_bottleneck_breakdown(rows, col_axis, row_axis, facet_axes, plots_dir, num_events):
    """One stacked-bar breakdown plot per row_axis value (variant).

    Each plot shows a single variant; x-axis = col_axis; one stacked bar per
    col value; segments = I/O / Decompress / Other. The global Y scale is shared
    across all plots so they are directly comparable.
    """
    if not rows or "read_wall_ms" not in rows[0] or "unzip_wall_ms" not in rows[0]:
        return None
    if all(float(r["read_wall_ms"]) == 0 and float(r["unzip_wall_ms"]) == 0 for r in rows):
        return None

    row_vals = distinct(rows, row_axis)

    # Compute global Y max across ALL variants so the plots share the same scale.
    global_max = 0.0
    for r in rows:
        io_ms   = float(r["read_wall_ms"])
        unz_ms  = float(r["unzip_wall_ms"])
        wall_ms = float(r["wall_s_mean"]) * 1000.0
        oth     = max(0.0, wall_ms - io_ms - unz_ms)
        global_max = max(global_max, io_ms + unz_ms + oth)

    pngs = []
    for rv in row_vals:
        variant_rows = [r for r in rows if r[row_axis] == rv]
        png = _bottleneck_one_variant(
            variant_rows, rv, col_axis, facet_axes, plots_dir, num_events, global_max
        )
        pngs.append(png)
    return pngs
