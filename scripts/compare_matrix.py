#!/usr/bin/env python3
"""Read-benchmark pivot heatmaps + bottleneck breakdown. Logic lives in matrixplot/.

Usage (repo root, Spack OFF for system matplotlib):
    /usr/bin/python3 scripts/compare_matrix.py <run_dir> [--rows AXIS --cols AXIS --metric NAME]
"""
from matrixplot.cli import main

if __name__ == "__main__":
    main()
