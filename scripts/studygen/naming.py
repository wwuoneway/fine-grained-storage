"""Compact id tags for datasets and write-option knobs."""


def si(n: int) -> str:
    """Compact size tag: 20000 -> '20k', 2000000 -> '2M', 1500 -> '1500'."""
    if n and n % 1_000_000 == 0:
        return f"{n // 1_000_000}M"
    if n and n % 1_000 == 0:
        return f"{n // 1_000}k"
    return str(n)


def tag_mib(mib: float) -> str:
    """Page-size tag, 'p' for the point since it lands in paths: 0.5 -> '0p5mib'."""
    return f"{float(mib):g}".replace(".", "p") + "mib"


def root_file_for(variant: str) -> str:
    """Mirror writing/strategy_one/main.cpp:root_file_for."""
    return "strategy_one_shuffled.root" if variant == "shuffle" else "strategy_one.root"
