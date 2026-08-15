"""Write the concrete generation/writing/benchmark JSON configs to disk."""
import itertools
import json
import pathlib

from .expand import datasets, write_opts
from .naming import root_file_for

REPO = pathlib.Path(__file__).resolve().parents[2]
GEN_DIR = REPO / "configs" / "generation" / "auto-generated"
WRITE_DIR = REPO / "configs" / "writing" / "auto-generated"
BENCH_DIR = REPO / "configs" / "benchmarks" / "auto-generated"

# Generated inputs and written files are keyed by content, not by sweep, so a
# re-run can skip rebuilding them.
GEN_ROOT = "output/generation/study"
WRITE_ROOT = "output/writing/study"


def write_json(path: pathlib.Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2) + "\n")


def gen_dir(ds_id):
    return f"{GEN_ROOT}/{ds_id}"


def write_dir(ds_id, w_id):
    return f"{WRITE_ROOT}/{ds_id}/{w_id}"


def dataset_dir(sweep, ds_id):
    return f"{sweep}/{ds_id}"


def run_dir(sweep, ds_id, w_id):
    return f"{dataset_dir(sweep, ds_id)}/{ds_id}__{w_id}"


def emit_generation(ds_id, n, parts, defaults):
    """Full generation config (config.cpp requires every field)."""
    gd = defaults.get("generation", {})
    cfg = {
        "num_events": n,
        "particles": {"min": parts["min"], "max": parts["max"]},
        "seed": gd.get("seed", 42),
        "output_dir": gen_dir(ds_id),
        "position": gd["position"],
        "momentum": gd["momentum"],
    }
    write_json(GEN_DIR / f"{ds_id}.json", cfg)


def emit_writing(ds_id, w_id, w_opts, axes):
    cfg = {
        "gen_dir": gen_dir(ds_id),
        "output_root": write_dir(ds_id, w_id),
        "variants": axes["writing"]["variants"],
        "shuffle_seed": axes["writing"].get("shuffle_seed", 0),
    }
    if w_opts is not None:
        cfg["write_options"] = w_opts
    write_json(WRITE_DIR / f"{ds_id}__{w_id}.json", cfg)


def emit_benchmark(ds_id, w_id, axes):
    r = axes["reading"]
    variants = axes["writing"]["variants"]
    root = write_dir(ds_id, w_id)
    cases = []
    k = 0

    def as_list(key, default):
        v = r.get(key, default)
        return v if isinstance(v, list) else [v]

    # One selection for every case, not a crossed axis.
    products = r.get("products")

    # Distance is read by scatter only, so crossing it with all patterns would
    # emit identical copies of the ones that ignore it.
    pattern_params = []
    for ap in r["access_pattern"]:
        if ap == "scatter":
            pattern_params += [(ap, d) for d in as_list("scatter_distance", 1)]
        else:
            pattern_params.append((ap, 0))

    for variant, (ap, dist), cache, cc, imt in itertools.product(
        variants, pattern_params, r["cache"], r["cluster_cache"], r["implicit_mt"]
    ):
        k += 1
        ap_label = f"scatter-{dist}" if ap == "scatter" else ap
        os_cache = {
            "state": cache,
            "evict_method": "posix_fadvise" if cache == "cold" else "none",
        }
        if cache == "warm":
            os_cache["warmup"] = True
        input_data = {
            "root_file": f"{root}/{variant}/{root_file_for(variant)}",
            "manifest_file": f"{root}/manifest.json",
        }
        if products:
            input_data["products"] = products
        cases.append(
            {
                "enabled": True,
                "metadata": {
                    "benchmark_num": k,
                    "name": f"{ds_id} {w_id} {variant} {ap_label} {cache} cc-{cc} imt-{imt}",
                    "description": (
                        f"{variant} layout, {ap_label} access, {cache} cache, "
                        f"cluster_cache={cc}, implicit_mt={imt}"
                        + (f", products={'+'.join(products)}" if products else "")
                    ),
                    "variant": variant,
                },
                "input_data": input_data,
                "num_events": r["num_events"],
                "access_pattern": ap,
                "scatter_distance": dist,
                "scatter_seed": r.get("scatter_seed", 1234),
                "access_seed": r.get("access_seed", 1234),
                "repetitions": r["repetitions"],
                "os_cache": os_cache,
                "read_options": {
                    "cluster_cache": cc,
                    "implicit_mt": imt,
                },
            }
        )
    write_json(BENCH_DIR / f"{ds_id}__{w_id}.json", {"benchmarks": cases})


def list_datasets(axes, sweep) -> int:
    """Print one TSV row per dataset, for run_study_all.sh to consume.

    Columns: ds_id  gen_cfg  gen_dir  dataset_dir. No files are written.
    """
    for ds_id, _n, _parts in datasets(axes):
        gen_cfg = f"configs/generation/auto-generated/{ds_id}.json"
        print(f"{ds_id}\t{gen_cfg}\t{gen_dir(ds_id)}\t{dataset_dir(sweep, ds_id)}")
    return 0


def list_combos(axes, sweep) -> int:
    """Print one TSV row per (dataset, wopts) combo, for run_study_all.sh to consume.

    Columns: ds_id  wopts_id  write_cfg  bench_cfg  write_dir  run_dir. No files are written.
    """
    for ds_id, _n, _parts in datasets(axes):
        for w_id, _opts in write_opts(axes):
            print(
                "\t".join(
                    [
                        ds_id,
                        w_id,
                        f"configs/writing/auto-generated/{ds_id}__{w_id}.json",
                        f"configs/benchmarks/auto-generated/{ds_id}__{w_id}.json",
                        write_dir(ds_id, w_id),
                        run_dir(sweep, ds_id, w_id),
                    ]
                )
            )
    return 0
