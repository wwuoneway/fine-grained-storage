#!/usr/bin/env python3
"""Step-to-step growth across the dataset steps. Logic lives in matrixplot/growth_curves.py.

Takes a sweep root holding one directory per dataset. Writes one figure per page
size under <sweep_root>/growth/, with one panel per metric: each panel is a grid
of every access pattern against every step transition, coloured by percent change.

Usage (repo root, Spack OFF so matplotlib resolves against the venv):
    .venv/bin/python3 scripts/growth_curves.py <sweep_root> [--metric NAME ...]
"""
import argparse
from pathlib import Path

from matrixplot.data import METRICS, RATIO_METRICS
from matrixplot.growth_curves import plot_growth_curves


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sweep_root", type=Path, help="folder holding one dir per dataset")
    ap.add_argument("--metric", action="append", choices=list(METRICS),
                    help=f"panel to draw, repeatable (default: {' '.join(RATIO_METRICS)})")
    ap.add_argument("--outdir", type=Path, default=None, help="where the figures go")
    args = ap.parse_args()

    pngs = plot_growth_curves(args.sweep_root, args.metric or RATIO_METRICS, args.outdir)
    if not pngs:
        raise SystemExit("no data (need at least two dataset steps with a common pattern)")
    for png in pngs:
        print(f"growth curves -> {png}")


if __name__ == "__main__":
    main()
