"""CLI: emit all study configs from axes.json, or --list a tier's combos.

  studygen                     emit configs for every tier in axes.json
  studygen --axes FILE         use a different axes file
  studygen --list [TIER]       print the combo TSV run_study_all.sh consumes
"""
import json
import pathlib
import sys

from .emit import REPO, emit_benchmark, emit_generation, emit_writing, list_combos
from .expand import datasets, write_opts

AXES_PATH = REPO / "configs" / "study" / "axes.json"  # default; override with --axes


def main() -> int:
    args = sys.argv[1:]
    axes_path = AXES_PATH
    if args and args[0] == "--axes":
        axes_path = pathlib.Path(args[1]).resolve()
        args = args[2:]

    axes = json.loads(axes_path.read_text())

    if args and args[0] == "--list":
        tier_filter = args[1] if len(args) > 1 else None
        return list_combos(axes, tier_filter)

    gen_root = axes["output_roots"]["generation"]
    write_root = axes["output_roots"]["writing"]
    defaults = axes.get("defaults", {})

    produced = []
    for tier_name, tier in axes["tiers"].items():
        for ds_id, n, parts in datasets(tier):
            gpath = emit_generation(ds_id, n, parts, defaults, gen_root)
            produced.append(("generation", tier_name, gpath, ""))
            for w_id, w_opts in write_opts(tier):
                wpath = emit_writing(ds_id, w_id, w_opts, tier, gen_root, write_root)
                produced.append(("writing", tier_name, wpath, ""))
                bpath, nrows = emit_benchmark(ds_id, w_id, tier, write_root)
                produced.append(("benchmark", tier_name, bpath, f"{nrows} rows"))

    print(f"axes: {axes_path.relative_to(REPO)}")
    print(f"emitted {len(produced)} config file(s):\n")
    for kind, tier_name, path, note in produced:
        rel = path.relative_to(REPO)
        suffix = f"  ({note})" if note else ""
        print(f"  [{tier_name:5}] {kind:10} {rel}{suffix}")
    return 0
