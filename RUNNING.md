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
one, and launches the binaries into the environment itself.

```bash
scripts/run_study_all.sh configs/study/shared/axes-example.json
```

The axes file is the whole matrix: the datasets, and per dataset the generation,
writing and reading axes. Tracked examples live in `configs/study/shared/`;
`axes-example.json` is the annotated one. Copy it and edit it to change what
runs. A loose copy directly under `configs/study/` is gitignored, so that is
where a machine-tuned matrix belongs.

Generation and writing are reused when their config and the binary that produced
them are unchanged; the benchmarks always run. Delete a directory under
`output/generation/` or `output/writing/` to force that stage to rebuild.

## Output

```
output/benchmarks/reading-benchmarks/<timestamp>/<dataset>/<dataset>__<page_size>/
  run_info.json     machine specs + full config
  summary.csv       one row per benchmark, drives the plots
  columns.json      pages per column of the files read, from fgs_inspect_columns
  benchmark_<N>/    per-benchmark log, metadata, raw.csv
  analysis/         plots/ and csv/
```

The sweep-root figures put **events per page** on their x axis: the dataset's
event count over the page count of a particle-parameter column (px/py/pz), where
all but a handful of a file's pages live. That page count is only knowable from
the written file, so `columns.json` has to exist before those figures can be
drawn. `run_study_all.sh` writes it after each benchmark; runs made before it did
need a backfill:

```bash
./build/tools/fgs_inspect_columns <run-dir>...   # Spack activated
```

Re-plot one run with different axes or a single metric:

```bash
.venv/bin/python3 scripts/compare_matrix.py <run-dir> [--rows AXIS --cols AXIS --metric NAME]
```

## One stage by hand (Spack activated)

Each exe takes a config path. `run_study_all.sh` writes one per stage under
`configs/*/auto-generated/`, so run it once first and reuse those.

```bash
./build/generation/fgs_generate        configs/generation/auto-generated/<ds>.json
./build/writing/fgs_strategy_one       configs/writing/auto-generated/<ds>__<wopts>.json
./build/writing/fgs_verify             output/writing/study/<ds>/<wopts>   # --all checks every event
./build/benchmarks/read/fgs_read_bench configs/benchmarks/auto-generated/<ds>__<wopts>.json <run-dir>
./build/tools/fgs_inspect_columns      <run-dir>
```

`fgs_verify` needs the `.bin` present, which a reused run may have skipped; run
`fgs_generate` on that dataset's config first.

Inspect files with `rootls -l <file>.root` or `cat <manifest>.json`.
