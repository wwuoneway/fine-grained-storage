"""CLI: emit the study configs from an axes file, or list what it expands to.

All three modes take --axes and --sweep; run_study_all.sh is the only caller.

  studygen --axes FILE --sweep DIR                  emit the configs
  studygen --axes FILE --sweep DIR --list           one TSV row per combo
  studygen --axes FILE --sweep DIR --list-datasets  one TSV row per dataset
"""
import json
import pathlib
import sys

from .emit import (
    emit_benchmark,
    emit_generation,
    emit_writing,
    list_combos,
    list_datasets,
)
from .expand import datasets, write_opts


def main() -> int:
    args = sys.argv[1:]
    axes_path = None
    sweep = None
    mode = None
    while args:
        arg = args.pop(0)
        if arg == "--axes":
            axes_path = pathlib.Path(args.pop(0)).resolve()
        elif arg == "--sweep":
            sweep = args.pop(0)
        elif arg in ("--list", "--list-datasets"):
            mode = arg
        else:
            print(f"studygen: unknown argument '{arg}'", file=sys.stderr)
            return 2

    if axes_path is None or sweep is None:
        print("studygen: --axes FILE and --sweep DIR are both required", file=sys.stderr)
        return 2

    axes = json.loads(axes_path.read_text())
    try:
        return run(axes, sweep, mode)
    except ValueError as err:
        print(f"studygen: {err}", file=sys.stderr)
        return 2


def run(axes, sweep, mode) -> int:
    if mode == "--list-datasets":
        return list_datasets(axes, sweep)
    if mode == "--list":
        return list_combos(axes, sweep)

    defaults = axes.get("defaults", {})
    for ds_id, n, parts in datasets(axes):
        emit_generation(ds_id, n, parts, defaults)
        for w_id, w_opts in write_opts(axes):
            emit_writing(ds_id, w_id, w_opts, axes)
            emit_benchmark(ds_id, w_id, axes, n)
    return 0
