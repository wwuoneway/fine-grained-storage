"""Expand one tier's axes into dataset and write-option combos."""
import itertools

from .naming import si, tag_bytes


def datasets(tier):
    """Yield (dataset_id, num_events, particles) for each generation combo."""
    g = tier["generation"]
    for n, parts in itertools.product(g["num_events"], g["particles"]):
        yield f"s{si(n)}_p{parts['min']}-{parts['max']}", n, parts


def write_opts(tier):
    """Yield (wopts_id, write_options_or_None) for each writing combo.

    With no compression/cluster/page sweep there is a single 'default' combo.
    """
    w = tier["writing"]
    comps = w.get("compression")
    clusters = w.get("cluster_size_bytes")
    pages = w.get("page_size_bytes")
    if comps is None and clusters is None and pages is None:
        yield "default", None
        return
    comps = comps or ["default"]
    clusters = clusters or [0]
    pages = pages or [0]
    for comp, cl, pg in itertools.product(comps, clusters, pages):
        wid = f"{comp.replace(':', '')}_cl{tag_bytes(cl)}_pg{tag_bytes(pg)}"
        yield wid, {"compression": comp, "cluster_size_bytes": cl, "page_size_bytes": pg}
