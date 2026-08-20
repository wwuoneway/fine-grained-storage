#!/usr/bin/env bash
#
# Benchmark every tier with Spack active, then plot the runs it produced with
# Spack out of the way. Spack is activated only inside the phase-1 subshell and
# every python call goes through py_isolated, so the two never mix.

set -euo pipefail

if (( BASH_VERSINFO[0] < 4 )); then
  echo "needs bash 4 or newer, found $BASH_VERSION" >&2
  exit 1
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Captured before anything activates Spack, so it is the clean PATH that phase 2
# restores. Refuse to start from a Spack shell, where it would not be clean.
BASE_PATH="$PATH"
if [[ -n "${SPACK_ENV:-}${SPACK_ROOT:-}" ]]; then
  echo "Spack is active in this shell; run from a shell without it" >&2
  exit 1
fi

if [[ -f "$REPO/.env" ]]; then
  # shellcheck disable=SC1091
  source "$REPO/.env"
else
  echo "missing $REPO/.env -- copy .env.example to .env and set the paths" >&2
  exit 1
fi
: "${FGS_SPACK_SETUP:?set FGS_SPACK_SETUP in .env}"
: "${FGS_SPACK_ENV:?set FGS_SPACK_ENV in .env}"

# Which study matrix to run. Defaults to axes.json; pass a variant as $1
# (e.g. configs/study/axes-1-event.json) to drive the whole pipeline from it.
AXES="${1:-configs/study/axes.json}"
[[ -f "$AXES" ]] || { echo "axes file not found: $AXES" >&2; exit 1; }

BUILD="$REPO/build"
GEN_EXE="$BUILD/generation/fgs_generate"
WRITE_EXE="$BUILD/writing/fgs_strategy_one"
READ_EXE="$BUILD/benchmarks/read/fgs_read_bench"
READ_BASE="output/benchmarks/reading-benchmarks"
for exe in "$GEN_EXE" "$WRITE_EXE" "$READ_EXE"; do
  [[ -x "$exe" ]] || { echo "missing executable: $exe -- build first" >&2; exit 1; }
done

# Pinning the measured binary keeps core placement constant across the sweep,
# which matters on a hybrid CPU.
PIN=()
if [[ -n "${FGS_BENCH_CPUS:-}" ]]; then
  command -v taskset >/dev/null 2>&1 \
    || { echo "FGS_BENCH_CPUS is set but taskset is not available" >&2; exit 1; }
  PIN=(taskset -c "$FGS_BENCH_CPUS")
fi

PLOT_PY="$REPO/.venv/bin/python3"
[[ -x "$PLOT_PY" ]] || {
  echo "missing $PLOT_PY -- run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2
  exit 1
}

# `env` has to be found by behaviour, not by name: uv installs a PATH-setup
# script also called `env` that ignores its arguments and exits 0.
resolve_env_bin() {
  local -a cands=()
  mapfile -t cands < <(type -aP env 2>/dev/null || true)
  cands+=(/usr/bin/env /bin/env)
  local cand probe
  for cand in "${cands[@]}"; do
    [[ -x "$cand" ]] || continue
    probe="$("$cand" -i FGS_ENV_PROBE=ok "$PLOT_PY" \
      -c 'import os; print(os.environ.get("FGS_ENV_PROBE", ""))' 2>/dev/null)" || continue
    [[ "$probe" == ok ]] || continue
    printf '%s\n' "$cand"
    return 0
  done
  return 1
}
ENV_BIN="$(resolve_env_bin || true)"
[[ -n "$ENV_BIN" ]] \
  || echo "no usable env found, isolating by unsetting Spack variables instead" >&2

# Every python call in this script runs through here, so none of them can pick up
# Spack's interpreter or its library paths.
py_isolated() {
  if [[ -n "$ENV_BIN" ]]; then
    "$ENV_BIN" -i PATH="$BASE_PATH" HOME="$HOME" "$PLOT_PY" "$@"
  else
    (
      unset PYTHONPATH PYTHONHOME LD_LIBRARY_PATH LD_PRELOAD
      PATH="$BASE_PATH"
      "$PLOT_PY" "$@"
    )
  fi
}

# exit 0 = manifest matches config = safe to skip this stage. The manifest
# stores the full generation config, so compare the whole block: any field
# (num_events, seed, particles, position, momentum, ...) that differs forces a
# regen. A field-by-field list here would silently skip position/momentum edits.
gen_matches() {  # $1 gen manifest.json   $2 generation config.json
  py_isolated - "$1" "$2" <<'PY'
import json, sys
man = json.load(open(sys.argv[1])).get("config", {})
cfg = json.load(open(sys.argv[2]))
sys.exit(0 if man == cfg else 1)
PY
}

write_matches() {  # $1 write manifest.json   $2 writing config.json
  py_isolated - "$1" "$2" <<'PY'
import json, sys
man = json.load(open(sys.argv[1]))
cfg = json.load(open(sys.argv[2]))
man_variants = sorted(v["name"] for v in man.get("variants", []))
cfg_variants = sorted(cfg["variants"])
seed_ok = all(
    v.get("shuffle_seed") == cfg["shuffle_seed"]
    for v in man.get("variants", [])
    if v["name"] == "shuffle"
)
# Write options change the physical layout, so reusing a .root across a change
# is wrong. Absent on both sides is "all ROOT defaults" and still matches.
opts_ok = man.get("write_options", {}) == cfg.get("write_options", {})
sys.exit(0 if (man_variants == cfg_variants and seed_ok and opts_ok) else 1)
PY
}

mapfile -t TIERS < <(
  py_isolated -c "import json,sys; [print(t) for t in json.load(open(sys.argv[1]))['tiers']]" "$AXES"
)
[[ ${#TIERS[@]} -gt 0 ]] || { echo "no tiers in $AXES" >&2; exit 1; }

# One dated folder per invocation; every config it runs becomes a named
# subfolder, so phase 2 plots exactly this sweep without dating each member.
SWEEP="$READ_BASE/$(date +%Y%m%d-%H%M%S)"

# Regenerate the study configs from the axes file so they can never be stale
# (they are gitignored / not tracked). No Spack needed, so keep it outside.
py_isolated scripts/gen_study_configs.py --axes "$AXES" >/dev/null

# Precompute every tier's combos up front so we know the total run count
# before starting; the counter below is what keeps a long run legible.
declare -A tier_combos
total=0
for tier in "${TIERS[@]}"; do
  combos="$(py_isolated scripts/gen_study_configs.py --axes "$AXES" --list "$tier")"
  [[ -n "$combos" ]] || { echo "no combos for tier '$tier'" >&2; exit 1; }
  tier_combos["$tier"]="$combos"
  total=$(( total + $(wc -l <<< "$combos") ))
done

echo "=== phase 1: benchmarks -- tiers: ${TIERS[*]} ==="
echo "--- $total benchmark(s) to run ---"
(
  # shellcheck disable=SC1090
  source "$FGS_SPACK_SETUP"
  spack env activate "$FGS_SPACK_ENV"

  done_count=0
  for tier in "${TIERS[@]}"; do
    combos="${tier_combos[$tier]}"

    while IFS=$'\t' read -r ds w gen_cfg write_cfg bench_cfg gen_out write_out; do
      [[ -n "$ds" ]] || continue
      done_count=$(( done_count + 1 ))
      echo "--- [$done_count/$total] $tier: $ds / $w ---"

      gen_changed=0
      if [[ -f "$gen_out/manifest.json" ]] && gen_matches "$gen_out/manifest.json" "$gen_cfg"; then
        echo "SKIP   gen    $ds"
      else
        echo "BUILD  gen    $ds"
        rm -rf "$gen_out"
        "$GEN_EXE" "$gen_cfg"
        gen_changed=1
      fi

      # A regen invalidates any .root written from the old inputs, so force the
      # write step whenever gen changed, even if its own config still matches.
      if [[ $gen_changed -eq 0 ]] \
         && [[ -f "$write_out/manifest.json" ]] \
         && write_matches "$write_out/manifest.json" "$write_cfg"; then
        echo "SKIP   write  $ds/$w"
      else
        echo "BUILD  write  $ds/$w"
        rm -rf "$write_out"
        "$WRITE_EXE" "$write_cfg"
      fi

      # posix_fadvise cannot evict pages that are still dirty.
      sync
      "${PIN[@]}" "$READ_EXE" "$bench_cfg" "$SWEEP/${ds}__${w}"
    done <<< "$combos"
  done
)

mapfile -t new_dirs < <(find "$SWEEP" -mindepth 1 -maxdepth 1 -type d | sort)
[[ ${#new_dirs[@]} -gt 0 ]] || { echo "no run dirs under $SWEEP" >&2; exit 1; }

echo
echo "=== phase 2: plots -- ${#new_dirs[@]} run dir(s) ==="
for d in "${new_dirs[@]}"; do
  echo "--- plotting $d ---"
  py_isolated scripts/compare_matrix.py "$d"
done

# One figure per metric across every run dir, since each holds a single max page size.
echo "--- locality curves across ${#new_dirs[@]} run dir(s) ---"
py_isolated scripts/locality_curves.py --outdir "$SWEEP" "${new_dirs[@]}" \
  || echo "locality curves skipped (no swept stride/scatter in these runs)"

echo
echo "=== done: benchmarked all tiers + plotted ${#new_dirs[@]} run dir(s) ==="
