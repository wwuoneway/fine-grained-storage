"""Build a (row x col) mean grid for one metric and write it as CSV."""
import csv
import math


def pivot(sel: list[dict], metric, row_axis, col_axis, row_vals, col_vals):
    """Mean of `metric` per (row_val, col_val) cell over the selected rows."""
    acc = {(i, j): [] for i in range(len(row_vals)) for j in range(len(col_vals))}
    for r in sel:
        i = row_vals.index(r[row_axis])
        j = col_vals.index(r[col_axis])
        acc[(i, j)].append(float(r[metric]))
    grid = [[math.nan] * len(col_vals) for _ in row_vals]
    for (i, j), vals in acc.items():
        if vals:
            grid[i][j] = sum(vals) / len(vals)
    return grid


def write_pivot_csv(path, grid, row_axis, col_axis, row_vals, col_vals, label=None):
    corner = f"{row_axis}\\{col_axis}" if label is None else f"{row_axis}\\{col_axis} [{label}]"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([corner] + col_vals)
        for i, rv in enumerate(row_vals):
            w.writerow([rv] + ["" if math.isnan(x) else x for x in grid[i]])
