# FGS2 on-disk format

Phase 1 of the *Fine-Grained Storage for the DUNE Experiment* benchmark produces
two binary dataset files (`positions.bin` and `momenta.bin`) plus a
`manifest.json`. The format is **lossless**, **self-describing**, and **per data
product**: all events' positions are in one file, all events' momenta in another.

This document is the authoritative spec. It must stay in agreement with the
constants at the top of
[`generation/include/fgs/bin_io.hpp`](../generation/include/fgs/bin_io.hpp).

## Why two files instead of one per event

One file per event at 1 million events = 1 million files. OS overhead (directory
lookups, inodes, `open()` syscalls) makes this impractical. Two files means two
`open()` calls total, and each file can be read in one sequential pass — cache
friendly for later benchmark phases.

## Conventions

- All multi-byte integers are **little-endian**.
- All floating-point values are **IEEE-754 `binary32`** (`float`), stored
  little-endian.
- No padding, no alignment gaps beyond the layout below.

## File header (80 bytes, same layout for both product files)

| Offset    | Type         | Bytes | Field          | Notes                                           |
|-----------|--------------|-------|----------------|-------------------------------------------------|
| `0`       | `char[4]`    | 4     | `magic`        | `'F','G','S','\0'` — identifies file type       |
| `4`       | `uint32`     | 4     | `version`      | `2` — format version; version field owns versioning, not magic |
| `8`       | `char[32]`   | 32    | `product_name` | null-padded ASCII, e.g. `"position"`            |
| `40`      | `char[32]`   | 32    | `product_type` | null-padded ASCII, e.g. `"float3"`              |
| `72`      | `uint64`     | 8     | `num_events`   | total number of events in this file             |
| **Total** |              | **80**|                |                                                 |

`product_name` and `product_type` are fixed-width 32-byte fields. Bytes beyond
the string content are zero-filled. A reader strips trailing null bytes to recover
the string.

## Body (immediately after the 80-byte header)

One **event record** per event, in event-id order (event 0 first):

| Offset within record | Type           | Bytes       | Field         | Notes                              |
|----------------------|----------------|-------------|---------------|------------------------------------|
| `0`                  | `uint32`       | 4           | `n_particles` | particle count N for this event    |
| `4`                  | `float32[3*N]` | `12*N`      | floats        | see below                          |

**Total record size = `4 + 12*N` bytes.**

When `N = 0` the record is exactly 4 bytes (the uint32 zero, no float bytes).

### Float layout per product file

**`positions.bin`:** floats are `x0, y0, z0, x1, y1, z1, ..., x_{N-1}, y_{N-1}, z_{N-1}` for the N particles of that event.

**`momenta.bin`:** floats are `px0, py0, pz0, px1, py1, pz1, ..., px_{N-1}, py_{N-1}, pz_{N-1}`.

Index `i` in positions and index `i` in momenta describe the **same particle**.

## Manifest (`manifest.json`)

Written alongside the product files. Fields:

| Field            | Type              | Description                                    |
|------------------|-------------------|------------------------------------------------|
| `format`         | string            | `"FGS2"` — identifies this format version      |
| `config`         | object            | the full config that produced this dataset      |
| `seed`           | uint64            | RNG seed used                                   |
| `num_events`     | uint64            | number of events                                |
| `positions_file` | string            | relative path to positions product file         |
| `momenta_file`   | string            | relative path to momenta product file           |
| `total_bytes`    | uint64            | combined on-disk size of both product files     |
| `total_particles`| uint64            | sum of n_particles across all events            |
| `events`         | array of objects  | one entry per event (see below)                 |

Each entry in `events`:

| Field        | Type   | Description                         |
|--------------|--------|-------------------------------------|
| `event_id`   | uint64 | event identifier (0-based)          |
| `n_particles`| uint32 | particle count for this event       |

The `n_particles` field lets a reader answer "how many particles in event N?"
without opening or scanning the binary product files.

## Versioning

If the data model changes (e.g. a third product is added), bump `version` in
`bin_io.hpp`. `ProductReader` will reject files with an unknown version with a
clear error message. Files with version `1` (the old per-event format) are also
detected and rejected with a specific message directing the user to re-run
generation.

## Round-tripping

`ProductWriter` and `ProductReader` are exact inverses: writing a dataset and
reading it back produces identical values (no loss, no rounding). Generation is
deterministic: the same config and seed produce byte-identical product files on
the same toolchain (`std::normal_distribution` is not pinned across stdlib
implementations, so byte-identity is guaranteed per-toolchain, not cross-platform).
