# Running the benchmark suite

Three stages: **generate** particle data (`generation/`), **write** it into
RNTuple layouts (`writing/`), **read**-benchmark them (`benchmarks/read/`).

## Setup (once)

```bash
cp .env.example .env                  # then set FGS_SPACK_SETUP and FGS_SPACK_ENV
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

## Build (Spack activated)

```bash
source "$FGS_SPACK_SETUP" && spack env activate "$FGS_SPACK_ENV"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Run the study (Spack NOT activated)

Use a shell where you have not activated Spack; the script refuses to start in
one, and launches the binaries into the environment itself. For numbers meant to
be reported, do the machine setup in [`BEFORE_RUN.md`](BEFORE_RUN.md) first.

```bash
scripts/run_study_all.sh configs/study/shared/axes-example.json
```

That one command covers generation, writing, every benchmark, the column
inspection the figures need, and all the plots. Nothing further down this page
has to be run by hand for a normal study.

The axes file is the whole matrix: the datasets, and per dataset the generation,
writing and reading axes. Tracked examples live in `configs/study/shared/`;
`axes-example.json` is the annotated one. Copy it and edit it to change what
runs. A loose copy directly under `configs/study/` is gitignored, so that is
where a machine-tuned matrix belongs.

Generation and writing are reused when their config and the binary that produced
them are unchanged; the benchmarks always run. Delete
`output/generation/study/<ds>` or `output/writing/study/<ds>` to force that
stage to rebuild.

## Output

One invocation writes one timestamped sweep root:

```
output/benchmarks/reading-benchmarks/<timestamp>/
  read_cost/events_per_page/     read-cost metrics against events per page
  read_cost/scatter_distance/    the same metrics against scatter distance
  heatmap/                       scatter distance against events per page, one grid per metric
  growth/                        percent change from each dataset step to the next
  <dataset>/                     one per dataset in the axes file
    *.png                        this dataset's metrics against access locality
    <dataset>__<page_size>/      one run: one dataset written at one max page size
      summary.csv                one row per benchmark, drives every plot
      run_info.json              machine specs + the full config
      columns.json               pages per column of the files read
      page_metrics_summary.md    page sizes and counts, per container
      benchmarks/benchmark_<N>/  per-benchmark log, metadata, summary, raw csv
      analysis/plots|csv/        this run's heatmaps and the pivots behind them
```

The figures above `<dataset>/` compare datasets, so they are drawn once every
dataset has landed. The ones inside are drawn as soon as that dataset finishes.

**Events per page** is the x axis of those cross-dataset figures: a dataset's
event count over the page count of a particle-parameter column (px/py/pz), where
all but a handful of a file's pages live. ROOT settles that count as it writes,
so it can only be read back off the file, which is what `columns.json` holds.

## One stage by hand (Spack activated)

Each exe takes a config path. `run_study_all.sh` writes one per stage under
`configs/*/auto-generated/`, so run it once first and reuse those.

```bash
./build/generation/fgs_generate        configs/generation/auto-generated/<ds>.json
./build/writing/fgs_strategy_one       configs/writing/auto-generated/<ds>__<wopts>.json
./build/writing/fgs_verify             --all output/writing/study/<ds>/<wopts>   # or --events 0,7,42
./build/benchmarks/read/fgs_read_bench configs/benchmarks/auto-generated/<ds>__<wopts>.json <run-dir>
./build/tools/fgs_inspect_columns      <run-dir>
```

`fgs_read_bench` does not write `columns.json` itself, so the inspector has to
follow it, and only after it: reading the descriptor back warms the page cache
the benchmark just evicted. A run dir without `columns.json` is skipped by the
cross-dataset figures, which name it on stderr.

`fgs_verify` needs the `.bin` present, which a reused run may have skipped; run
`fgs_generate` on that dataset's config first.

## Re-plot (Spack NOT activated)

Every figure can be redrawn from what is already on disk, with different axes or
a single metric:

```bash
.venv/bin/python3 scripts/compare_matrix.py    <run-dir> [--rows AXIS --cols AXIS --metric NAME]
.venv/bin/python3 scripts/locality_curves.py   --outdir <dataset-dir> <run-dir>...
.venv/bin/python3 scripts/event_size_curves.py <sweep-root> [--metric NAME ...]
.venv/bin/python3 scripts/scatter_curves.py    <sweep-root> [--metric NAME ...]
.venv/bin/python3 scripts/growth_curves.py     <sweep-root> [--metric NAME ...]
```

Inspect files with `rootls -l <file>.root` or `cat <manifest>.json`.
