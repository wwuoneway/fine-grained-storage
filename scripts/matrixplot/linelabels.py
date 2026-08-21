"""Direct line-end labels, replacing a legend: each line is labeled at a fixed
x reserved outside the data range, stacked apart in pixel space so labels never
overlap regardless of the axis scale, connected back to their line by a leader.
"""
from __future__ import annotations


def label_lines(ax, entries, label_x, min_gap_px=12, ha="right"):
    """entries: [(x, y, text, colour)], each the anchor point of one line."""
    disp = [(ax.transData.transform((x, y))[1], x, y, text, colour)
            for x, y, text, colour in entries]
    disp.sort(key=lambda d: d[0])
    placed = []
    inv = ax.transData.inverted()
    for py, x, y, text, colour in disp:
        p = py
        while any(abs(p - q) < min_gap_px for q in placed):
            p += min_gap_px
        placed.append(p)
        _, label_y = inv.transform((0, p))
        arrow = dict(arrowstyle="-", color=colour, lw=0.5, shrinkA=0, shrinkB=3, alpha=0.6)
        ax.annotate(text, xy=(x, y), xytext=(label_x, label_y), textcoords="data",
                    ha=ha, va="center", fontsize=7.5, color=colour,
                    annotation_clip=False, arrowprops=arrow)


def despine(ax):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.spines["left"].set_color("0.3")
    ax.spines["bottom"].set_color("0.3")
    ax.tick_params(top=False, right=False, which="both")
    ax.grid(True, axis="y", which="major", color="0.88", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
