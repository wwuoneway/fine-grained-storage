"""Render one metric's faceted heatmap and write its pivot CSVs."""
import itertools
import math

import matplotlib.pyplot as plt

from .data import DIRECTION, distinct, fmt
from .pivot import pivot, write_pivot_csv


def draw_panel(ax, grid, metric, row_vals, col_vals, cmap, title, vmin, vmax):
    im = ax.imshow(grid, aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax)
    ax.set_xticks(range(len(col_vals)), col_vals, rotation=20, ha="right", fontsize=8)
    ax.set_yticks(range(len(row_vals)), row_vals, fontsize=8)
    ax.set_title(title, fontsize=8)
    # Annotate each cell with its value -- always black so it stays readable on
    # any colormap shade.
    for i in range(len(row_vals)):
        for j in range(len(col_vals)):
            v = grid[i][j]
            label = "--" if math.isnan(v) else fmt(metric, v)
            ax.text(j, i, label, ha="center", va="center", color="black", fontsize=8)
    return im


def make_metric(rows, metric, row_axis, col_axis, facet_axes, plots_dir, csv_dir, num_events):
    row_vals = distinct(rows, row_axis)
    col_vals = distinct(rows, col_axis)
    facet_vals = [distinct(rows, a) for a in facet_axes]
    combos = list(itertools.product(*facet_vals)) if facet_axes else [()]

    direction, cmap = DIRECTION.get(metric, ("", "viridis"))

    n = len(combos)
    ncols = min(n, 3) if n > 1 else 1
    nrows = math.ceil(n / ncols)
    fig, axs = plt.subplots(nrows, ncols, figsize=(4.6 * ncols, 3.6 * nrows), squeeze=False)
    fig.subplots_adjust(wspace=0.35, hspace=0.55)

    # All grids up front so every panel shares one color scale; otherwise the
    # single colorbar only describes the last panel. An empty facet combo
    # (non-crossed sweep) yields an all-NaN grid.
    grids = []
    for combo in combos:
        sel = [r for r in rows if all(r[a] == v for a, v in zip(facet_axes, combo))]
        nes = {r["num_events"] for r in sel}
        assert len(nes) <= 1, f"num_events not constant for panel {combo}: {nes}"

        grid = pivot(sel, metric, row_axis, col_axis, row_vals, col_vals)
        facetkey = "_".join(f"{a}-{v}" for a, v in zip(facet_axes, combo)) or "all"
        write_pivot_csv(
            csv_dir / f"{metric}__{facetkey}.csv", grid, row_axis, col_axis, row_vals, col_vals
        )
        grids.append(grid)

    flat = [x for grid in grids for row in grid for x in row if not math.isnan(x)]
    vmin, vmax = (min(flat), max(flat)) if flat else (0.0, 1.0)

    last_im = None
    for idx, (combo, grid) in enumerate(zip(combos, grids)):
        panel_title = "\n".join(f"{a}={v}" for a, v in zip(facet_axes, combo)) or "(all)"
        ax = axs[idx // ncols][idx % ncols]
        # Only the outer panels get axis labels, to avoid inner-panel clutter.
        if idx % ncols == 0:
            ax.set_ylabel(row_axis, fontsize=8)
        if idx // ncols == nrows - 1:
            ax.set_xlabel(col_axis, fontsize=8)
        last_im = draw_panel(ax, grid, metric, row_vals, col_vals, cmap, panel_title, vmin, vmax)

    # Blank any unused grid cells.
    for idx in range(n, nrows * ncols):
        axs[idx // ncols][idx % ncols].axis("off")

    if last_im is not None:
        fig.colorbar(last_im, ax=axs, shrink=0.7, label=metric)
    fig.suptitle(
        f"{metric}  ({direction})   [num_events={num_events}]   "
        f"rows={row_axis} x cols={col_axis}",
        fontsize=10,
    )
    png = plots_dir / f"{metric}.png"
    fig.savefig(png, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return png, n
