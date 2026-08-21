"""Expand the axes file into dataset and write-option combos."""
from .naming import si, tag_mib


def datasets(axes):
    """Return (dataset_id, num_events, particles) for each `generation` entry."""
    out = []
    seen = set()
    for entry in axes["generation"]:
        n = entry["num_events"]
        parts = entry["particles"]
        ds_id = f"s{si(n)}_p{parts['min']}-{parts['max']}"
        if ds_id in seen:
            raise ValueError(f"two generation entries give dataset id '{ds_id}'")
        seen.add(ds_id)
        out.append((ds_id, n, parts))
    return out


# ROOT's own MaxUnzippedPageSize (RNTupleWriteOptions.hxx: 1024 * 1024). Asking
# for it explicitly would write a byte-identical file under a second id, so this
# value maps to the 'default' combo.
ROOT_DEFAULT_MAX_PAGE_MIB = 1


def write_opts(axes):
    """Yield (wopts_id, write_options_or_None) for each writing combo.

    An absent `max_page_size_mib`, or an entry equal to ROOT's own default,
    yields the 'default' combo: no write_options at all, so every RNTuple
    option keeps its ROOT default.
    """
    pages = axes["writing"].get("max_page_size_mib")
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
