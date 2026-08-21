#!/usr/bin/env python3
"""Read-cost metrics against scatter distance. Logic lives in matrixplot/scatter_curves.py.

Takes a sweep root holding one directory per dataset. Writes, per page size: one
line-plot figure under <sweep_root>/read_cost/ (one panel per metric, one curve
per dataset) and one heatmap grid per metric under <sweep_root>/heatmap/
(rows are events per page, columns are scatter distance).

Usage (repo root, Spack OFF so matplotlib resolves against the venv):
    .venv/bin/python3 scripts/scatter_curves.py <sweep_root> [--metric NAME ...]
"""
import argparse
from pathlib import Path

from matrixplot.data import METRICS, RATIO_METRICS
from matrixplot.scatter_curves import plot_scatter_curves, plot_scatter_heatmap


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sweep_root", type=Path, help="folder holding one dir per dataset")
    ap.add_argument("--metric", action="append", choices=list(METRICS),
                    help=f"panel to draw, repeatable (default: {' '.join(RATIO_METRICS)})")
    ap.add_argument("--outdir", type=Path, default=None, help="where the figures go")
    args = ap.parse_args()

    metrics = args.metric or RATIO_METRICS
    pngs = plot_scatter_curves(args.sweep_root, metrics, args.outdir)
    pngs += plot_scatter_heatmap(args.sweep_root, metrics, args.outdir)
    if not pngs:
        raise SystemExit("no data (need several datasets with a manifest, a summary.csv "
                         "and a columns.json each, and a swept scatter distance)")
    for png in pngs:
        print(f"scatter distance curves -> {png}")


if __name__ == "__main__":
    main()
