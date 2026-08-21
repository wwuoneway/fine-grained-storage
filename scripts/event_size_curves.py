#!/usr/bin/env python3
"""Read-cost metrics against events per page. Logic lives in matrixplot/event_size.py.

Takes a sweep root holding one directory per dataset, each carrying the
columns.json fgs_inspect_columns writes. Writes one figure per page size under
<sweep_root>/read_cost/, with one panel per metric.

Usage (repo root, Spack OFF so matplotlib resolves against the venv):
    .venv/bin/python3 scripts/event_size_curves.py <sweep_root> [--metric NAME ...]
"""
import argparse
from pathlib import Path

from matrixplot.data import METRICS, RATIO_METRICS
from matrixplot.event_size import plot_event_size_curves


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sweep_root", type=Path, help="folder holding one dir per dataset")
    ap.add_argument("--metric", action="append", choices=list(METRICS),
                    help=f"panel to draw, repeatable (default: {' '.join(RATIO_METRICS)})")
    ap.add_argument("--outdir", type=Path, default=None, help="where the figures go")
    args = ap.parse_args()

    pngs = plot_event_size_curves(args.sweep_root, args.metric or RATIO_METRICS, args.outdir)
    if not pngs:
        raise SystemExit("no data (need several datasets with a manifest, a summary.csv "
                         "and a columns.json each)")
    for png in pngs:
        print(f"events per page curves -> {png}")


if __name__ == "__main__":
    main()
