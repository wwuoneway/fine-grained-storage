"""Expand one tier's axes into dataset and write-option combos."""
import itertools

from .naming import si, tag_mib


def datasets(tier):
    """Yield (dataset_id, num_events, particles) for each generation combo."""
    g = tier["generation"]
    for n, parts in itertools.product(g["num_events"], g["particles"]):
        yield f"s{si(n)}_p{parts['min']}-{parts['max']}", n, parts


# ROOT's own MaxUnzippedPageSize (RNTupleWriteOptions.hxx: 1024 * 1024). Asking
# for it explicitly would write a byte-identical file under a second id, so this
# value maps to the 'default' combo and reuses whatever 'default' already wrote.
ROOT_DEFAULT_MAX_PAGE_MIB = 1


def write_opts(tier):
    """Yield (wopts_id, write_options_or_None) for each writing combo.

    An absent `max_page_size_mib`, or an entry equal to ROOT's own default,
    yields the 'default' combo: no write_options at all, so every RNTuple
    option keeps its ROOT default.
    """
    pages = tier["writing"].get("max_page_size_mib")
    if not pages:
        yield "default", None
        return
    for mib in pages:
        if not isinstance(mib, (int, float)) or isinstance(mib, bool) or mib <= 0:
            raise ValueError(f"max_page_size_mib entries must be positive numbers, got {mib!r}")
        if mib == ROOT_DEFAULT_MAX_PAGE_MIB:
            yield "default", None
        else:
            yield f"pg{tag_mib(mib)}", {"max_page_size_mib": mib}
