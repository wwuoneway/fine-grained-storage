#!/usr/bin/env bash
#
# Run the whole study from one axes file: for each dataset, write and
# read-benchmark every page size, then plot. Generation and writing are reused
# when their inputs have not changed; the benchmarks always run.
#
# Usage: run_study_all.sh <axes.json>

set -euo pipefail

readonly READ_BASE="output/benchmarks/reading-benchmarks"
readonly STAMP_FILE=".stage-key"
readonly CONFIG_GENERATOR="scripts/gen_study_configs.py"

die() {
  printf 'run_study_all: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '%s\n' "$*"
}

step() {
  printf '\n=== %s ===\n' "$*"
}

require_file() {
  [[ -f "$1" ]] || die "${2:-missing file}: $1"
}

require_executable() {
  [[ -x "$1" ]] || die "${2:-missing executable}: $1"
}

require_directory() {
  [[ -d "$1" ]] || die "${2:-missing directory}: $1"
}

load_dotenv() {
  local env_file="$REPO/.env"
  require_file "$env_file" "missing .env -- copy .env.example and set the paths"
  # shellcheck disable=SC1090
  source "$env_file"
  : "${FGS_SPACK_SETUP:?set FGS_SPACK_SETUP in .env}"
  : "${FGS_SPACK_ENV:?set FGS_SPACK_ENV in .env}"
}

# Pinning the measured binary keeps core placement constant across the sweep,
# which matters on a hybrid CPU.
resolve_cpu_pinning() {
  PIN=()
  [[ -n "${FGS_BENCH_CPUS:-}" ]] || return 0
  command -v taskset >/dev/null 2>&1 \
    || die "FGS_BENCH_CPUS is set but taskset is not available"
  PIN=(taskset -c "$FGS_BENCH_CPUS")
}

check_prerequisites() {
  local axes="$1"

  (( BASH_VERSINFO[0] >= 4 )) || die "needs bash 4 or newer, found $BASH_VERSION"

  # Checked before init_spack, which sets SPACK_ROOT itself. An already-active
  # environment would leak its interpreter into $PLOT_PY.
  [[ -z "${SPACK_ENV:-}" ]] \
    || die "Spack environment '$SPACK_ENV' is active; run from a shell without one"

  load_dotenv
  require_file "$axes" "axes file not found"

  BUILD="$REPO/build"
  GEN_EXE="$BUILD/generation/fgs_generate"
  WRITE_EXE="$BUILD/writing/fgs_strategy_one"
  READ_EXE="$BUILD/benchmarks/read/fgs_read_bench"
  local exe
  for exe in "$GEN_EXE" "$WRITE_EXE" "$READ_EXE"; do
    require_executable "$exe" "missing executable -- build first"
  done

  PLOT_PY="$REPO/.venv/bin/python3"
  require_executable "$PLOT_PY" \
    "missing plotting venv -- run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt"
  require_file "$CONFIG_GENERATOR" "missing config generator"

  resolve_cpu_pinning

  GEN_DIGEST="$(sha256sum "$GEN_EXE" | cut -d' ' -f1)"
  WRITE_DIGEST="$(sha256sum "$WRITE_EXE" | cut -d' ' -f1)"
}

init_spack() {
  # shellcheck disable=SC1090
  source "$FGS_SPACK_SETUP" \
    || die "cannot source FGS_SPACK_SETUP: $FGS_SPACK_SETUP"
  command -v spack >/dev/null 2>&1 \
    || die "spack is not on PATH after sourcing $FGS_SPACK_SETUP"

  local env_dir
  env_dir="$(spack location -e "$FGS_SPACK_ENV")" \
    || die "unknown Spack environment: $FGS_SPACK_ENV"
  SPACK_VIEW="$env_dir/.spack-env/view"
  require_directory "$SPACK_VIEW" "Spack environment has no view"
}

# Scoped to the launched binary so the plotting virtualenv never sees Spack. The
# exes carry a RUNPATH here already; this covers a build linked without one.
run_in_env() {
  LD_LIBRARY_PATH="$SPACK_VIEW/lib/root:$SPACK_VIEW/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$@"
}

studygen() {
  local axes="$1" sweep="$2"
  shift 2
  "$PLOT_PY" "$CONFIG_GENERATOR" --axes "$axes" --sweep "$sweep" "$@"
}

# Reuse needs the config, the binary consuming it, and everything upstream to be
# unchanged; the upstream key is folded in so a changed dataset rewrites the .root.
stage_key() {  # $1 config  $2 exe digest  [$3 upstream key]
  "$PLOT_PY" -c '
import hashlib, json, sys
canon = json.dumps(json.load(open(sys.argv[1])), sort_keys=True, separators=(",", ":"))
print(hashlib.sha256((canon + sys.argv[2] + sys.argv[3]).encode()).hexdigest())
' "$1" "$2" "${3-}"
}

stage_is_current() {  # $1 output dir  $2 expected key
  local recorded
  [[ -r "$1/$STAMP_FILE" ]] || return 1
  read -r recorded < "$1/$STAMP_FILE"
  [[ "$recorded" == "$2" ]]
}

mark_stage_current() {  # $1 output dir  $2 key
  require_directory "$1" "stage produced no output directory"
  printf '%s\n' "$2" > "$1/$STAMP_FILE"
}

# Deferred until a write actually needs the .bin, so they can be deleted to
# reclaim space while the .root files stay reusable.
ensure_dataset_generated() {
  local ds="$1" gen_cfg="$2" gen_dir="$3" gen_key="$4"

  [[ -z "${GENERATED_DATASETS[$ds]:-}" ]] || return 0

  if stage_is_current "$gen_dir" "$gen_key"; then
    info "reuse  gen    $ds"
  else
    rm -rf -- "$gen_dir"
    run_in_env "$GEN_EXE" "$gen_cfg"
    mark_stage_current "$gen_dir" "$gen_key"
  fi
  GENERATED_DATASETS["$ds"]=1
}

write_root_files() {
  local ds="$1" wopts="$2" write_cfg="$3" write_dir="$4" write_key="$5"

  if stage_is_current "$write_dir" "$write_key"; then
    info "reuse  write  $ds/$wopts"
    return 0
  fi

  rm -rf -- "$write_dir"
  run_in_env "$WRITE_EXE" "$write_cfg"
  mark_stage_current "$write_dir" "$write_key"
}

run_benchmark() {  # $1 bench cfg  $2 run dir
  # posix_fadvise cannot evict pages that are still dirty.
  sync
  run_in_env ${PIN[@]+"${PIN[@]}"} "$READ_EXE" "$1" "$2"
}

# Between benchmarks, never during one, so plotting cannot perturb a measurement
# while still making each dataset readable as soon as it lands.
plot_dataset() {
  local ds="$1" dataset_dir="$2"
  shift 2
  local -a run_dirs=("$@")

  info "--- plots for $ds ---"
  local run_dir
  for run_dir in "${run_dirs[@]}"; do
    "$PLOT_PY" scripts/compare_matrix.py "$run_dir"
  done
  "$PLOT_PY" scripts/locality_curves.py --outdir "$dataset_dir" "${run_dirs[@]}" \
    || info "locality curves skipped for $ds (no swept scatter)"
}

process_dataset() {
  local ds="$1" gen_cfg="$2" gen_dir="$3" dataset_dir="$4"

  step "dataset $ds"

  local gen_key
  gen_key="$(stage_key "$gen_cfg" "$GEN_DIGEST")"

  local -a run_dirs=()
  local combo_ds wopts write_cfg bench_cfg write_dir run_dir write_key
  while IFS=$'\t' read -r combo_ds wopts write_cfg bench_cfg write_dir run_dir; do
    [[ "$combo_ds" == "$ds" ]] || continue

    DONE_COUNT=$(( DONE_COUNT + 1 ))
    info "--- [$DONE_COUNT/$TOTAL] $ds / $wopts ---"

    write_key="$(stage_key "$write_cfg" "$WRITE_DIGEST" "$gen_key")"
    if ! stage_is_current "$write_dir" "$write_key"; then
      ensure_dataset_generated "$ds" "$gen_cfg" "$gen_dir" "$gen_key"
    fi
    write_root_files "$ds" "$wopts" "$write_cfg" "$write_dir" "$write_key"

    run_benchmark "$bench_cfg" "$run_dir"
    run_dirs+=("$run_dir")
  done <<< "$COMBOS"

  (( ${#run_dirs[@]} > 0 )) || die "no benchmarks matched dataset $ds"
  plot_dataset "$ds" "$dataset_dir" "${run_dirs[@]}"
}

main() {
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  cd "$REPO" || die "cannot enter repo root: $REPO"

  local axes="${1:-}"
  [[ -n "$axes" ]] || die "usage: run_study_all.sh <axes.json>"

  check_prerequisites "$axes"
  init_spack

  local sweep
  sweep="$READ_BASE/$(date +%Y%m%d-%H%M%S)"

  # Emitted fresh every run so the configs can never describe a different sweep.
  studygen "$axes" "$sweep" >/dev/null || die "failed to emit study configs from $axes"

  local -a datasets=()
  mapfile -t datasets < <(studygen "$axes" "$sweep" --list-datasets)
  (( ${#datasets[@]} > 0 )) || die "no datasets in $axes"

  COMBOS="$(studygen "$axes" "$sweep" --list)"
  [[ -n "$COMBOS" ]] || die "no combos in $axes"
  TOTAL="$(wc -l <<< "$COMBOS")"
  DONE_COUNT=0
  declare -gA GENERATED_DATASETS=()

  step "${#datasets[@]} dataset(s), $TOTAL benchmark(s) -> $sweep"

  local row ds gen_cfg gen_dir dataset_dir
  for row in "${datasets[@]}"; do
    IFS=$'\t' read -r ds gen_cfg gen_dir dataset_dir <<< "$row"
    process_dataset "$ds" "$gen_cfg" "$gen_dir" "$dataset_dir"
  done

  step "done: $sweep"
}

main "$@"
