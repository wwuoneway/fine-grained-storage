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


def write_json(path: pathlib.Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2) + "\n")


def emit_generation(ds_id, n, parts, defaults, gen_root):
    """Full generation config (config.cpp requires every field)."""
    gd = defaults.get("generation", {})
    cfg = {
        "num_events": n,
        "particles": {"min": parts["min"], "max": parts["max"]},
        "seed": gd.get("seed", 42),
        "output_dir": f"{gen_root}/{ds_id}",
        "position": gd["position"],
        "momentum": gd["momentum"],
    }
    path = GEN_DIR / f"{ds_id}.json"
    write_json(path, cfg)
    return path


def emit_writing(ds_id, w_id, w_opts, tier, gen_root, write_root):
    cfg = {
        "gen_dir": f"{gen_root}/{ds_id}",
        "output_root": f"{write_root}/{ds_id}/{w_id}",
        "variants": tier["writing"]["variants"],
        "shuffle_seed": tier["writing"].get("shuffle_seed", 0),
    }
    if w_opts is not None:
        cfg["write_options"] = w_opts
    path = WRITE_DIR / f"{ds_id}__{w_id}.json"
    write_json(path, cfg)
    return path


def emit_benchmark(ds_id, w_id, tier, write_root):
    r = tier["reading"]
    variants = tier["writing"]["variants"]
    root = f"{write_root}/{ds_id}/{w_id}"
    cases = []
    k = 0
    # Distance and stride are each read by one pattern only, so each is crossed
    # with that pattern alone: a full cross would emit N identical copies of
    # every pattern that ignores the value.
    def as_list(key, default):
        v = r.get(key, default)
        return v if isinstance(v, list) else [v]

    pattern_params = []
    for ap in r["access_pattern"]:
        if ap == "scatter":
            pattern_params += [(ap, d, 0) for d in as_list("scatter_distance", 1)]
        elif ap == "strided":
            pattern_params += [(ap, 0, s) for s in as_list("stride", 16)]
        else:
            pattern_params.append((ap, 0, 0))

    for variant, (ap, dist, stride), cache, cc, imt in itertools.product(
        variants, pattern_params, r["cache"], r["cluster_cache"], r["implicit_mt"]
    ):
        k += 1
        if ap == "scatter":
            ap_label = f"scatter-{dist}"
        elif ap == "strided":
            ap_label = f"strided-{stride}"
        else:
            ap_label = ap
        os_cache = {
            "state": cache,
            "evict_method": "posix_fadvise" if cache == "cold" else "none",
        }
        if cache == "warm":
            os_cache["warmup"] = True
        cases.append(
            {
                "enabled": True,
                "metadata": {
                    "benchmark_num": k,
                    "name": f"{ds_id} {w_id} {variant} {ap_label} {cache} cc-{cc} imt-{imt}",
                    "description": (
                        f"{variant} layout, {ap_label} access, {cache} cache, "
                        f"cluster_cache={cc}, implicit_mt={imt}"
                    ),
                    "variant": variant,
                },
                "input_data": {
                    "root_file": f"{root}/{variant}/{root_file_for(variant)}",
                    "manifest_file": f"{root}/manifest.json",
                },
                "num_events": r["num_events"],
                "access_pattern": ap,
                "scatter_distance": dist,
                "scatter_seed": r.get("scatter_seed", 1234),
                "stride": stride,
                "access_seed": r.get("access_seed", 1234),
                "repetitions": r["repetitions"],
                "os_cache": os_cache,
                "read_options": {
                    "cluster_cache": cc,
                    "implicit_mt": imt,
                },
            }
        )
    path = BENCH_DIR / f"{ds_id}__{w_id}.json"
    write_json(path, {"benchmarks": cases})
    return path, len(cases)


def list_combos(axes, tier_filter) -> int:
    """Print one TSV row per (dataset, wopts) combo, for run_study_all.sh to consume.

    Columns: ds_id  wopts_id  gen_cfg  write_cfg  bench_cfg  gen_out_dir  write_out_dir
    All paths are relative to the repo root. No files are written.
    """
    gen_root = axes["output_roots"]["generation"]
    write_root = axes["output_roots"]["writing"]
    for tier_name, tier in axes["tiers"].items():
        if tier_filter and tier_name != tier_filter:
            continue
        for ds_id, _n, _parts in datasets(tier):
            for w_id, _opts in write_opts(tier):
                print(
                    "\t".join(
                        [
                            ds_id,
                            w_id,
                            f"configs/generation/auto-generated/{ds_id}.json",
                            f"configs/writing/auto-generated/{ds_id}__{w_id}.json",
                            f"configs/benchmarks/auto-generated/{ds_id}__{w_id}.json",
                            f"{gen_root}/{ds_id}",
                            f"{write_root}/{ds_id}/{w_id}",
                        ]
                    )
                )
    return 0
