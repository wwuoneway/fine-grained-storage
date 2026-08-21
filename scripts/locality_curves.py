#!/usr/bin/env python3
"""Access-locality cost curves across run dirs. Logic lives in matrixplot/locality.py.

Takes several run dirs because each holds one max page size, and the figures
draw one curve per page size. One figure per metric.

Usage (repo root, Spack OFF so matplotlib resolves against the venv):
    .venv/bin/python3 scripts/locality_curves.py --outdir DIR <run_dir> [<run_dir> ...]
"""
import argparse
from pathlib import Path

from matrixplot.locality import plot_locality_curves


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dirs", nargs="+", type=Path, help="run folders containing summary.csv")
    ap.add_argument("--outdir", type=Path, default=Path("."), help="where the figures go")
    args = ap.parse_args()

    pngs = plot_locality_curves(args.run_dirs, args.outdir)
    if not pngs:
        raise SystemExit("no locality data (need max page size metadata + swept scatter)")
    for png in pngs:
        print(f"locality curves -> {png}")


if __name__ == "__main__":
    main()
