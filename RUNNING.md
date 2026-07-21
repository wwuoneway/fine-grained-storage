# Running the benchmark suite

The pipeline has three stages: **generate** synthetic particle data
(`generation/`), **write** it into RNTuple in several layouts (`writing/`, one
folder per variant), and **read**-benchmark every variant across cache states
and access patterns (`benchmarks/read/`). `writing/fgs_verify` cross-checks each
variant back against the generated binaries.

---

## 0. Activate ROOT (once per shell)

ROOT 6.40.02 comes from a Spack environment. The paths are machine-specific, so
they live in `.env` (copy `.env.example`): `SPACK_SETUP` is the Spack
`setup-env.sh` to source, `SPACK_ENV` the environment providing ROOT 6.40.

```bash
source "$SPACK_SETUP" && spack env activate "$SPACK_ENV"
root-config --version            # expect 6.40.02
```

CMake finds ROOT automatically once `root-config` is on the PATH; no paths are
hardcoded in any `CMakeLists.txt`.

---

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Produces `build/{generation/fgs_generate, writing/fgs_strategy_one,
writing/fgs_verify, benchmarks/read/fgs_read_bench}`.

---

## 2. Run the study (one command)

`configs/study/axes.json` is the single source of truth for the study matrix
(tiers, and per tier the generation / writing / reading axes). To change what
runs, edit `axes.json`; `scripts/run_study_all.sh` regenerates the per-stage
configs from it (via `scripts/gen_study_configs.py`) and drives every stage.

```bash
scripts/run_study_all.sh          # needs .env
```

It benchmarks every tier with Spack on, then plots every run with Spack off,
keeping the two environments from colliding. Variants `axes-5000.json` /
`axes-10000.json` reuse the same dataset and only raise the read `num_events`, so
no regeneration is needed (`gen_study_configs.py --axes configs/study/axes-5000.json`).

Each read run is a self-contained timestamped folder:

```
output/benchmarks/reading-benchmarks/<YYYYmmdd-HHMMSS>/
  run_info.json          machine specs + full config
  summary.csv            one aggregated row per benchmark (drives the plots)
  benchmark_<N>/         per-benchmark log, metadata, raw.csv, per-rep reports
  analysis/plots/        pivot heatmaps + wall-time bottleneck breakdown
  analysis/csv/          the pivot CSVs behind the plots
```

Plotting alone (defaults to the newest run) needs Spack **off** so it uses the
system matplotlib that Spack's Python shadows:

```bash
python3 scripts/compare_matrix.py output/benchmarks/reading-benchmarks/<timestamp>
```

---

## 3. Running a single stage by hand (optional)

Each executable takes an optional config path and falls back to a default. Useful
for one-offs; the study runner calls the same exes on generated configs.

```bash
./build/generation/fgs_generate    configs/generation/default.json     # generate
./build/writing/fgs_strategy_one   configs/writing/strategy_one.json   # write variants
./build/writing/fgs_verify                                             # verify all variants
./build/benchmarks/read/fgs_read_bench configs/benchmarks/reading_benchmarks.json
```

- Generation is deterministic (seed 42); skip it if `output/generation/` is
  already populated and the dataset size is unchanged.
- `fgs_verify` samples events 42 and the last by default; `--all` checks every
  event (slower). Both layout variants passing shows the index makes physical
  row order irrelevant to reads.

Inspect output directly with `rootls -l <file>.root` or `cat <manifest>.json`.
