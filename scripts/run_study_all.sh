#!/usr/bin/env bash
#
# run_study_all.sh  -- benchmark every tier (Spack ON), then plot every run it
# produced (Spack OFF). Spack is only activated inside the phase-1 subshell, and
# plots run under `env -i`, so the two opposite environments never collide.
#
# Per (dataset, wopts) combo: fgs_generate -> fgs_strategy_one -> sync ->
# fgs_read_bench. Generate/write are skipped when their manifest already matches
# the config; reads always make a fresh timestamped run dir. The sync flushes
# dirty pages so cold-cache eviction (posix_fadvise) actually evicts them.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

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

PLOT_PY="$REPO/.venv/bin/python3"
[[ -x "$PLOT_PY" ]] || {
  echo "missing $PLOT_PY -- run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2
  exit 1
}

# exit 0 = manifest matches config = safe to skip this stage. The manifest
# stores the full generation config, so compare the whole block: any field
# (num_events, seed, particles, position, momentum, ...) that differs forces a
# regen. A field-by-field list here would silently skip position/momentum edits.
gen_matches() {  # $1 gen manifest.json   $2 generation config.json
  python3 - "$1" "$2" <<'PY'
import json, sys
man = json.load(open(sys.argv[1])).get("config", {})
cfg = json.load(open(sys.argv[2]))
sys.exit(0 if man == cfg else 1)
PY
}

write_matches() {  # $1 write manifest.json   $2 writing config.json
  python3 - "$1" "$2" <<'PY'
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
  /usr/bin/python3 -c "import json,sys; [print(t) for t in json.load(open(sys.argv[1]))['tiers']]" "$AXES"
)
[[ ${#TIERS[@]} -gt 0 ]] || { echo "no tiers in $AXES" >&2; exit 1; }

# Marker mtime pins "now"; find -newer later grabs only this run's dirs.
marker="$(mktemp)"
trap 'rm -f "$marker"' EXIT

echo "=== phase 1: benchmarks -- tiers: ${TIERS[*]} ==="
(
  # shellcheck disable=SC1090
  source "$FGS_SPACK_SETUP"
  spack env activate "$FGS_SPACK_ENV"

  # Regenerate the study configs from axes.json so they can never be stale
  # (they are gitignored / not tracked).
  python3 scripts/gen_study_configs.py --axes "$AXES" >/dev/null

  # Precompute every tier's combos up front so we know the total run count
  # before starting; the counter below is what keeps a long run legible.
  declare -A tier_combos
  total=0
  for tier in "${TIERS[@]}"; do
    combos="$(python3 scripts/gen_study_configs.py --axes "$AXES" --list "$tier")"
    [[ -n "$combos" ]] || { echo "no combos for tier '$tier'" >&2; exit 1; }
    tier_combos["$tier"]="$combos"
    total=$(( total + $(wc -l <<< "$combos") ))
  done
  echo "--- $total benchmark(s) to run ---"

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

      sync
      "$READ_EXE" "$bench_cfg"
    done <<< "$combos"
  done
)

mapfile -t new_dirs < <(
  find "$READ_BASE" -mindepth 1 -maxdepth 1 -type d -newer "$marker" | sort
)
[[ ${#new_dirs[@]} -gt 0 ]] || { echo "no new run dirs under $READ_BASE" >&2; exit 1; }

echo
echo "=== phase 2: plots -- ${#new_dirs[@]} run dir(s) ==="
for d in "${new_dirs[@]}"; do
  echo "--- plotting $d ---"
  env -i PATH=/usr/bin:/bin HOME="$HOME" \
    "$PLOT_PY" scripts/compare_matrix.py "$d"
done

echo
echo "=== done: benchmarked all tiers + plotted ${#new_dirs[@]} run dir(s) ==="
