"""Turn a run's summary.csv into per-metric heatmaps + a bottleneck breakdown.

Rows/cols default to access_pattern x variant (override with --rows/--cols),
faceted over the remaining axes that vary. num_events is held fixed across the
comparison. Outputs land in <run_dir>/analysis/{plots,csv}/.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from .bottleneck import plot_bottleneck_breakdown
from .data import (
    AXES,
    DEFAULT_METRICS,
    distinct,
    read_summary,
    run_cluster_page_summary,
    run_generation_summary,
    run_max_page_size,
    run_payload_mib,
)
from .heatmap import make_metric
from .page_metrics import write_page_metrics_md


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("run_dir", help="a run folder containing summary.csv")
    ap.add_argument("--rows", default="access_pattern", choices=AXES, help="pivot row axis")
    ap.add_argument("--cols", default="variant", choices=AXES, help="pivot column axis")
    ap.add_argument("--metric", default=None, help="single metric (default: all present)")
    args = ap.parse_args()

    run_dir = Path(args.run_dir)
    rows = read_summary(run_dir / "summary.csv")
    if not rows:
        raise SystemExit(f"no rows in {run_dir / 'summary.csv'}")
    if args.rows == args.cols:
        raise SystemExit("--rows and --cols must differ")

    # Hold num_events fixed across the whole comparison (apples-to-apples).
    all_ne = {r["num_events"] for r in rows}
    if len(all_ne) != 1:
        raise SystemExit(f"summary mixes num_events {all_ne}; filter to one value first")
    num_events = next(iter(all_ne))

    if args.metric:
        if args.metric not in rows[0]:
            raise SystemExit(
                f"metric \"{args.metric}\" is not a summary.csv column "
                f"(available: {', '.join(m for m in DEFAULT_METRICS if m in rows[0])})"
            )
        metrics = [args.metric]
    else:
        metrics = [m for m in DEFAULT_METRICS if m in rows[0]]

    # Facet over the remaining sweep axes that actually vary (>1 value).
    facet_axes = [
        a for a in AXES if a not in (args.rows, args.cols) and len(distinct(rows, a)) > 1
    ]

    # This run's own analysis folder: <run_dir>/analysis/{plots,csv}.
    out_base = run_dir / "analysis"
    plots_dir = out_base / "plots"
    csv_dir = out_base / "csv"
    plots_dir.mkdir(parents=True, exist_ok=True)
    csv_dir.mkdir(parents=True, exist_ok=True)

    print(f"run       : {run_dir}")
    print(f"pivot     : rows={args.rows}  cols={args.cols}  facets={facet_axes or '(none)'}")
    print(f"num_events: {num_events}  (held fixed)")
    print(f"outputs   : {out_base}/  (plots/ + csv/)\n")
    for metric in metrics:
        png, n = make_metric(
            rows, metric, args.rows, args.cols, facet_axes, plots_dir, csv_dir, num_events
        )
        print(f"  {metric:24} -> {png.relative_to(out_base)}  ({n} panel(s) + {n} pivot CSV(s))")

    payload_mib = run_payload_mib(run_dir)
    cluster_page_summary = run_cluster_page_summary(run_dir)
    generation_summary = run_generation_summary(run_dir)
    max_page_size = run_max_page_size(run_dir)
    pngs = plot_bottleneck_breakdown(rows, args.cols, args.rows, facet_axes, plots_dir, num_events,
                                     payload_mib, cluster_page_summary, generation_summary,
                                     max_page_size)
    if pngs:
        for png in pngs:
            print(f"  {'bottleneck_breakdown':24} -> {png.relative_to(out_base)}")
    else:
        print("  bottleneck_breakdown     -> skipped (counters absent or all-zero)")

    md_path = write_page_metrics_md(run_dir)
    if md_path:
        print(f"  {'page_metrics_summary':24} -> {md_path.relative_to(run_dir)}")
    else:
        print("  page_metrics_summary    -> skipped (n_page_read absent from summary.csv)")
