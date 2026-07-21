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
: "${SPACK_SETUP:?set SPACK_SETUP in .env}"
: "${SPACK_ENV:?set SPACK_ENV in .env}"

BUILD="$REPO/build"
GEN_EXE="$BUILD/generation/fgs_generate"
WRITE_EXE="$BUILD/writing/fgs_strategy_one"
READ_EXE="$BUILD/benchmarks/read/fgs_read_bench"
READ_BASE="output/benchmarks/reading-benchmarks"
for exe in "$GEN_EXE" "$WRITE_EXE" "$READ_EXE"; do
  [[ -x "$exe" ]] || { echo "missing executable: $exe -- build first" >&2; exit 1; }
done

# exit 0 = manifest matches config = safe to skip this stage.
gen_matches() {  # $1 gen manifest.json   $2 generation config.json
  python3 - "$1" "$2" <<'PY'
import json, sys
man = json.load(open(sys.argv[1])).get("config", {})
cfg = json.load(open(sys.argv[2]))
ok = (
    man.get("num_events") == cfg["num_events"]
    and man.get("seed") == cfg["seed"]
    and man.get("particles", {}).get("min") == cfg["particles"]["min"]
    and man.get("particles", {}).get("max") == cfg["particles"]["max"]
)
sys.exit(0 if ok else 1)
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
sys.exit(0 if (man_variants == cfg_variants and seed_ok) else 1)
PY
}

mapfile -t TIERS < <(
  /usr/bin/python3 -c "import json; [print(t) for t in json.load(open('configs/study/axes.json'))['tiers']]"
)
[[ ${#TIERS[@]} -gt 0 ]] || { echo "no tiers in configs/study/axes.json" >&2; exit 1; }

# Marker mtime pins "now"; find -newer later grabs only this run's dirs.
marker="$(mktemp)"
trap 'rm -f "$marker"' EXIT

echo "=== phase 1: benchmarks -- tiers: ${TIERS[*]} ==="
(
  # shellcheck disable=SC1090
  source "$SPACK_SETUP"
  spack env activate "$SPACK_ENV"

  # Regenerate the study configs from axes.json so they can never be stale
  # (they are gitignored / not tracked).
  python3 scripts/gen_study_configs.py >/dev/null

  for tier in "${TIERS[@]}"; do
    combos="$(python3 scripts/gen_study_configs.py --list "$tier")"
    [[ -n "$combos" ]] || { echo "no combos for tier '$tier'" >&2; exit 1; }

    while IFS=$'\t' read -r ds w gen_cfg write_cfg bench_cfg gen_out write_out; do
      [[ -n "$ds" ]] || continue
      echo "--- $tier: $ds / $w ---"

      if [[ -f "$gen_out/manifest.json" ]] && gen_matches "$gen_out/manifest.json" "$gen_cfg"; then
        echo "SKIP   gen    $ds"
      else
        echo "BUILD  gen    $ds"
        "$GEN_EXE" "$gen_cfg"
      fi

      if [[ -f "$write_out/manifest.json" ]] && write_matches "$write_out/manifest.json" "$write_cfg"; then
        echo "SKIP   write  $ds/$w"
      else
        echo "BUILD  write  $ds/$w"
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
    /usr/bin/python3 scripts/compare_matrix.py "$d"
done

echo
echo "=== done: benchmarked all tiers + plotted ${#new_dirs[@]} run dir(s) ==="
