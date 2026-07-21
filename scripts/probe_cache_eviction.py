#!/usr/bin/env python3
"""
Confirm that posix_fadvise(POSIX_FADV_DONTNEED) actually drops a file from
the OS page cache on this machine and filesystem.

The benchmark relies on this syscall to enforce a cold-cache state before
each repetition.  This script verifies the assumption empirically: it times
a raw byte-read of a file (no ROOT, no decode) before and after eviction.
If eviction worked, the cold read is slower because it must fault from disk.

Usage (run from the repo root):
    python3 scripts/probe_cache_eviction.py [FILE] [--runs N]

    FILE    file to probe; defaults to the no-shuffle RNTuple of the smoke
            dataset (output/writing/study/s20k_p2-10/default/no-shuffle/strategy_one.root)
    --runs  timed reads per state, default 5; we keep best-of-N to reduce
            noise from unrelated I/O on the machine

Requires: Python 3, Linux (posix_fadvise is Linux-only).
"""

import argparse
import os
import time

DEFAULT_FILE = "output/writing/study/s20k_p2-10/default/no-shuffle/strategy_one.root"
CHUNK = 1 << 20  # 1 MiB read buffer


def read_bytes(path: str) -> float:
    """Return wall-clock seconds to read the whole file, no decode."""
    fd = os.open(path, os.O_RDONLY)
    try:
        t = time.perf_counter()
        while os.read(fd, CHUNK):
            pass
        return time.perf_counter() - t
    finally:
        os.close(fd)


def evict(path: str) -> None:
    """Drop the file's clean pages from the page cache."""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", default=DEFAULT_FILE)
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()

    if not os.path.exists(args.file):
        raise SystemExit(
            f"file not found: {args.file}\n"
            f"Run from the repo root, or pass a path explicitly."
        )

    size_mb = os.path.getsize(args.file) / 1e6
    print(f"file  : {args.file}")
    print(f"size  : {size_mb:.1f} MB")
    print(f"runs  : {args.runs} per state (best-of-N reported)\n")

    # prime the cache, then time reads that come entirely from RAM
    read_bytes(args.file)
    warm = [read_bytes(args.file) for _ in range(args.runs)]

    # evict before every read so each must fault from disk
    cold = []
    for _ in range(args.runs):
        evict(args.file)
        cold.append(read_bytes(args.file))

    bw = min(warm)
    bc = min(cold)

    print(f"warm  (ms): {[round(t * 1e3, 1) for t in warm]}")
    print(f"  best = {bw * 1e3:.1f} ms  |  {size_mb / bw / 1e3:.1f} GB/s  (RAM)")
    print(f"cold  (ms): {[round(t * 1e3, 1) for t in cold]}")
    print(f"  best = {bc * 1e3:.1f} ms  |  {size_mb / bc / 1e3:.1f} GB/s  (disk)\n")

    delta_ms = (bc - bw) * 1e3
    ratio = bc / bw
    print(f"cold - warm = {delta_ms:.1f} ms   ({ratio:.1f}x slower cold)")

    if ratio >= 1.3:
        print("=> eviction works: cold reads are measurably slower.")
    else:
        print("=> no clear difference: fadvise may be a no-op on this filesystem,")
        print("   or the file is small enough that re-faulting is negligible.")


if __name__ == "__main__":
    main()
