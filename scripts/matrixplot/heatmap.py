"""Render one metric's faceted heatmap and write its pivot CSVs."""
import itertools
import math

import matplotlib.pyplot as plt

from .data import METRICS, distinct, fmt
from .grid import panel_grid
from .pivot import pivot, write_pivot_csv


def draw_panel(ax, grid, metric, row_vals, col_vals, cmap, title, vmin, vmax):
    im = ax.imshow(grid, aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax)
    ax.set_xticks(range(len(col_vals)), col_vals, rotation=20, ha="right")
    ax.set_yticks(range(len(row_vals)), row_vals)
    ax.set_title(title)
    # Annotate each cell with its value -- always black so it stays readable on
    # any colormap shade.
    for i in range(len(row_vals)):
        for j in range(len(col_vals)):
            v = grid[i][j]
            label = "--" if math.isnan(v) else fmt(metric, v)
            ax.text(j, i, label, ha="center", va="center", color="black")
    return im


def make_metric(rows, metric, row_axis, col_axis, facet_axes, plots_dir, csv_dir,
                num_events, render=True):
    """Write this metric's pivot CSVs, and its heatmap when render is set."""
    row_vals = distinct(rows, row_axis)
    col_vals = distinct(rows, col_axis)
    facet_vals = [distinct(rows, a) for a in facet_axes]
    combos = list(itertools.product(*facet_vals)) if facet_axes else [()]

    spec = METRICS[metric]
    # NaN cells as light gray: transparent-over-white on RdYlGn reads as a
    # mid-scale value.
    cmap = plt.get_cmap(spec.cmap).copy()
    cmap.set_bad("#e8e8e8")

    n = len(combos)

    # All grids up front so every panel shares one color scale; otherwise the
    # single colorbar only describes the last panel. An empty facet combo
    # (non-crossed sweep) yields an all-NaN grid.
    grids = []
    for combo in combos:
        sel = [r for r in rows if all(r[a] == v for a, v in zip(facet_axes, combo))]
        nes = {r["num_events"] for r in sel}
        assert len(nes) <= 1, f"num_events not constant for panel {combo}: {nes}"

        grid = pivot(sel, metric, row_axis, col_axis, row_vals, col_vals)
        if spec.scale != 1.0:
            grid = [[v * spec.scale for v in gr] for gr in grid]
        facetkey = "_".join(f"{a}-{v}" for a, v in zip(facet_axes, combo)) or "all"
        write_pivot_csv(
            csv_dir / f"{spec.slug}__{facetkey}.csv", grid, row_axis, col_axis,
            row_vals, col_vals, spec.label
        )
        grids.append(grid)

    if not render:
        return None, n

    fig, axes = panel_grid(n, panel_w=4.6, panel_h=3.6)
    fig.subplots_adjust(wspace=0.35, hspace=0.55)
    flat = [x for grid in grids for row in grid for x in row if not math.isnan(x)]
    vmin, vmax = (min(flat), max(flat)) if flat else (0.0, 1.0)

    last_im = None
    ncols = min(n, 3) if n > 1 else 1
    nrows = math.ceil(n / ncols)
    for idx, (combo, grid) in enumerate(zip(combos, grids)):
        panel_title = "\n".join(f"{a}={v}" for a, v in zip(facet_axes, combo)) or "(all)"
        ax = axes[idx]
        # Only the outer panels get axis labels, to avoid inner-panel clutter.
        if idx % ncols == 0:
            ax.set_ylabel(row_axis)
        if idx // ncols == nrows - 1:
            ax.set_xlabel(col_axis)
        last_im = draw_panel(ax, grid, metric, row_vals, col_vals, cmap, panel_title, vmin, vmax)

    if last_im is not None:
        fig.colorbar(last_im, ax=axes, shrink=0.7, label=spec.label)
    fig.suptitle(
        f"{spec.label}  ({spec.direction})   [num_events={num_events}]   "
        f"rows={row_axis} x cols={col_axis}"
    )
    png = plots_dir / f"{spec.slug}.png"
    fig.savefig(png)
    plt.close(fig)
    return png, n
