"""Panel grid shared by the heatmap and bottleneck figures."""
from __future__ import annotations

import math

import matplotlib.pyplot as plt


def panel_grid(n: int, panel_w: float, panel_h: float, max_cols: int = 3):
    """(fig, axes) for n panels, laid out at most max_cols wide.

    Returns exactly n axes; any leftover cell in the last row is hidden.
    """
    ncols = min(n, max_cols) if n > 1 else 1
    nrows = math.ceil(n / ncols)
    fig, axs = plt.subplots(
        nrows, ncols, figsize=(panel_w * ncols, panel_h * nrows), squeeze=False
    )
    axes = [axs[i // ncols][i % ncols] for i in range(n)]
    for i in range(n, nrows * ncols):
        axs[i // ncols][i % ncols].axis("off")
    return fig, axes
