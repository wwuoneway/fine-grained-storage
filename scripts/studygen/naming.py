"""Compact id tags for datasets and write-option knobs."""


def si(n: int) -> str:
    """Compact size tag: 20000 -> '20k', 2000000 -> '2M', 1500 -> '1500'."""
    if n and n % 1_000_000 == 0:
        return f"{n // 1_000_000}M"
    if n and n % 1_000 == 0:
        return f"{n // 1_000}k"
    return str(n)


def tag_bytes(b: int) -> str:
    """Storage-knob tag: 0 -> 'D' (ROOT default), else a compact byte count."""
    if b == 0:
        return "D"
    if b % (1 << 20) == 0:
        return f"{b >> 20}m"
    if b % (1 << 10) == 0:
        return f"{b >> 10}k"
    return str(b)


def root_file_for(variant: str) -> str:
    """Mirror writing/strategy_one/main.cpp:root_file_for."""
    return "strategy_one_shuffled.root" if variant == "shuffle" else "strategy_one.root"
