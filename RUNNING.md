# Running the benchmark suite

Quickstart for building and running both phases:

- **Generation** (`generation/`) — produces synthetic particle data as flat
  binary files in `output/generation/`.
- **Writing strategies** (`writing/`) — each strategy writes that data into ROOT
  RNTuple in its own layout. `strategy_one` emits one folder per write variant
  under `output/writing/rntuple/strategy_one/`.
- **Verification** (`writing/fgs_verify.cpp`) — reads each variant back through
  the index and cross-checks it against the Phase 1 binaries.

---

## 0. Prerequisites — activate ROOT (one time per shell)

ROOT 6.38 comes from a Spack environment. These two commands are
machine-specific (they point at this workstation's Spack install); on another
machine, activate ROOT 6.38 however it is installed there.

```bash
source /home/ahmed/install/phlex-work-dir/spack/share/spack/setup-env.sh
spack env activate my-phlex-environment
```

Sanity check — must print `6.38.x`:

```bash
root-config --version
```

CMake finds ROOT automatically once `root-config` is on the PATH. No Spack
paths are hardcoded in any `CMakeLists.txt`.

---

## 1. Configure + build (once)

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This builds:

| Executable | Path |
|---|---|
| `fgs_generate`     | `build/generation/fgs_generate` |
| `fgs_strategy_one` | `build/writing/fgs_strategy_one` |
| `fgs_verify`       | `build/writing/fgs_verify` |

To rebuild just the strategy_one writer after editing it:

```bash
cmake --build build --target fgs_strategy_one -j$(nproc)
```

---

## 2. Phase 1 — generate the data

```bash
./build/generation/fgs_generate configs/generation/default.json
```

- Argument is optional; with none it uses `configs/generation/default.json`.
- Dataset size is controlled here — edit `num_events` in that config (or point at
  a different config) and regenerate to get a smaller/larger set.
- Output lands in `output/generation/`:
  ```
  output/generation/positions.bin   output/generation/momenta.bin   output/generation/manifest.json
  ```
- Deterministic: seed 42 → identical bytes every run.

Skip this step if `output/generation/positions.bin` already exists and you do not
need to regenerate.

---

## 3. Phase 2a — write to RNTuple with `strategy_one`

```bash
./build/writing/fgs_strategy_one configs/writing/strategy_one.json
```

- Argument is optional; with none it uses `configs/writing/strategy_one.json`.
- Loads each data product (position, momentum) independently, then emits **one
  folder per write variant** listed in the config, plus **one** manifest for the
  whole strategy at the root:
  ```
  output/writing/rntuple/strategy_one/
    manifest.json                         product registry + variant list (one per strategy)
    no-shuffle/strategy_one.root          (events in event order)
    shuffle/strategy_one_shuffled.root    (events in a seeded shuffle; distinct name)
  ```
  Each variant's ROOT file holds the position + momentum RNTuples and the shared
  index TTree; the single manifest lists the products and the variants (each with
  its file path, on-disk size, and the shuffle seed on the shuffled one).
- The shuffle (`shuffle_seed`) only changes the physical row layout; the index
  makes reads order-independent, so both variants return identical per-event data.
- Prints per-variant file size and wall time.

---

## 4. Phase 2b — verify every variant

```bash
./build/writing/fgs_verify
# or: ./build/writing/fgs_verify [--all] <strategy_output_root> <gen_dir>
```

- Reads the single strategy `manifest.json` at the output root, then verifies
  every variant it lists (`no-shuffle` and `shuffle`), opening each via the
  variant's `file` path from the manifest.
- For sample events (42 and the last) it does an O(log N) `TTreeIndex`
  `GetEntryNumberWithIndex` lookup, reads only that event's rows from the
  `position` and `momentum` RNTuples, and cross-checks every value against the
  Phase 1 binaries (exact equality). Aborts loudly on any mismatch.
- `--all` verifies **every** event instead of just the samples — an exhaustive
  integrity check (slower on large datasets).
- Both variants passing against the same reference data demonstrates the index
  makes physical layout (ordered vs shuffled) irrelevant to reads.

---

## 5. Full demo from scratch (copy-paste)

```bash
# activate ROOT
source /home/ahmed/install/phlex-work-dir/spack/share/spack/setup-env.sh
spack env activate my-phlex-environment
root-config --version            # expect 6.38.x

# build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# run the pipeline
./build/generation/fgs_generate    configs/generation/default.json    # Phase 1
./build/writing/fgs_strategy_one   configs/writing/strategy_one.json  # Phase 2a (write)
./build/writing/fgs_verify                                            # Phase 2b (verify all variants)
```

---

## 6. Inspecting the output by hand (optional)

```bash
rootls -l output/writing/rntuple/strategy_one/no-shuffle/strategy_one.root
cat       output/writing/rntuple/strategy_one/manifest.json
```
