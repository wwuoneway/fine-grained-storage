# strategy_one RNTuple layout

`strategy_one` reads the FGS2 binary dataset (see [`FORMAT.md`](FORMAT.md)) and
writes it into ROOT files using **RNTuple**, ROOT's columnar storage format. This
document is the authoritative schema for that output. It must stay in agreement
with the field definitions in
[`writing/strategy_one/main.cpp`](../writing/strategy_one/main.cpp).

Producer: `fgs_strategy_one`. Consumer / cross-check reader: `fgs_verify`.

---

## 1. What is produced

`strategy_one` emits one folder per **write variant** under its output root, plus
**one** manifest for the whole strategy at the root (the product registry is the
same for every variant, so it is not duplicated). The variants differ only in the
physical order events are written; the schema is identical. With the default config:

```
output/writing/rntuple/strategy_one/
  manifest.json               product registry + variant list (one per strategy)
  no-shuffle/                 events written in event order (0,1,2,...)
    strategy_one.root         one ROOT TFile: 2 data RNTuples + 1 index TTree
  shuffle/                    events written in a seeded shuffle (e.g. 3,10,1,5,...)
    strategy_one.root
```

A "data product" here is one physics quantity per particle. There are two:
`position` and `momentum`. Each product gets one **data** RNTuple. Both share a
single combined **index** — a TTree named `index` (not an RNTuple).

```
        strategy_one.root  (TFile)
        ┌─────────────────────────────────────────────────────────── ┐
        │   position     momentum     RNTuples (the physics numbers) │
        │   index        TTree        (event,product) -> row address │
        └─────────────────────────────────────────────────────────── ┘
                              ▲
                              │  described by
              manifest.json ──┘  (which products exist, where they live)
```

### Write variants (the ordering axis)

The variants exist to study how on-disk event order affects reads — in DUNE
events do not arrive sorted.

- **`no-shuffle`** — events written in event order; each event's rows land in one
  ascending block.
- **`shuffle`** — events written in a permutation produced by a seeded shuffle
  (`shuffle_seed` in the config). The same permutation is used for both products.

The shuffle changes **only** the physical row layout. Each event's particles are
still contiguous, and the index maps `event_id` to its rows regardless of order,
so a reader gets identical results from either variant.

---

## 2. Data RNTuples — one row per particle

`position` and `momentum` have the same shape; only the column names differ.

```
position RNTuple                          momentum RNTuple
 columns:                                  columns:
   event_id : uint64                         event_id : uint64
   x        : float                          px       : float
   y        : float                          py       : float
   z        : float                          pz       : float

 1 row  =  1 particle                      1 row  =  1 particle
```

Within a variant, each event occupies one **contiguous block** of rows. In
`no-shuffle` those blocks appear in ascending event order; in `shuffle` the
blocks appear in shuffled order, but each block is still internally contiguous.

`event_id` is repeated on every row on purpose — it makes each row
self-describing and lets `fgs_verify` assert that the rows it read really belong
to the event it asked for, independently of the index.

### Worked example (`no-shuffle`)

Three events with particle counts 2, 3, 2. The `position` RNTuple holds:

```
position RNTuple  (logical row view)
 row │ event_id │    x     │    y     │    z
 ────┼──────────┼──────────┼──────────┼──────────
  0  │    0     │  12.31   │  -5.63   │  301.20    ┐ event 0
  1  │    0     │ -64.39   │  53.56   │ -322.52    ┘ (2 particles)
  2  │    1     │   8.10   │  19.44   │  -77.01    ┐
  3  │    1     │ -12.55   │  -3.20   │  140.06    │ event 1
  4  │    1     │  64.77   │  41.13   │  -9.88     ┘ (3 particles)
  5  │    2     │  21.76   │ -57.48   │ -302.83    ┐ event 2
  6  │    2     │  -6.15   │  26.12   │  139.40    ┘ (2 particles)
```

In `shuffle` the same three blocks would appear in a shuffled order (say event
1, then 2, then 0), so `row_start` values differ — but each event's block is
unchanged and the index records the new ranges.

### Columnar, not row-major

RNTuple does not store the table row-by-row. It stores each column as its own
compressed stream. Logically it is the table above; physically it is:

```
 event_id stream:  [ 0, 0, 1, 1, 1, 2, 2 ]
 x stream:         [ 12.31, -64.39, 8.10, -12.55, 64.77, 21.76, -6.15 ]
 y stream:         [ -5.63, 53.56, 19.44, -3.20, 41.13, -57.48, 26.12 ]
 z stream:         [ 301.20, -322.52, -77.01, 140.06, -9.88, -302.83, 139.40 ]
```

A reader that only wants `x` reads only the `x` stream.

---

## 3. Index TTree — one row per (event, product) (the address book)

A single TTree named `index` maps each `(event_id, product)` pair to the
contiguous row range that event occupies in that product's data RNTuple. It is a
**TTree**, not an RNTuple, because TTree provides a persistent value-based index
(`BuildIndex`) for O(log N) lookup without scanning — RNTuple has no such index.

```
index TTree
 branches:
   event_id       : uint64   which event
   product_id     : uint64   numeric product key: 0 = position, 1 = momentum
   product_name   : string   "position" or "momentum" (human-readable)
   container_name : string   the data RNTuple to read ("position"/"momentum")
   row_start      : uint64   first data row for this event in that container
   row_count      : uint64   number of data rows (= number of particles)

 1 row  =  1 (event, product) pair      (TWO rows per event)
```

`product_id` is numeric because `TTreeIndex` only supports numeric major/minor
keys — a string branch is evaluated as a pointer and produces garbage. The
`product_id -> name` mapping is recorded in the manifest so readers don't
hard-code it.

For the worked example above (3 events), the `index` TTree is:

```
index TTree
 row │ event_id │ product_id │ product_name │ row_start │ row_count
 ────┼──────────┼────────────┼──────────────┼───────────┼───────────
  0  │    0     │     0      │ "position"   │     0     │     2
  1  │    0     │     1      │ "momentum"   │     0     │     2
  2  │    1     │     0      │ "position"   │     2     │     3
  3  │    1     │     1      │ "momentum"   │     2     │     3
  4  │    2     │     0      │ "position"   │     5     │     2
  5  │    2     │     1      │ "momentum"   │     5     │     2
```

The range is half-open: event `e` owns data rows `[row_start, row_start + row_count)`.

After all rows are filled, the writer calls
`index_tree->BuildIndex("event_id", "product_id")`, which stores a persistent
two-key `TTreeIndex` inside the file. A reader jumps straight to one
(event, product) row with `GetEntryNumberWithIndex(event_id, product_id)` — a
direct O(log N) lookup, no forward scan.

> Note on `shuffle`: the index TTree rows are filled in write order, so in the
> shuffled variant the *physical* index rows are also shuffled — but `BuildIndex`
> makes lookup value-based, so order does not matter to a reader.

### How the index points into the data

```
   index TTree (position rows)            position (data)
 ┌──────────────────────────┐            ┌─────────────────────┐
 │ event 0 : start=0 count=2 │──────────▶│ row 0  (event 0)    │
 │                           │      └───▶│ row 1  (event 0)    │
 │ event 1 : start=2 count=3 │──────────▶│ row 2  (event 1)    │
 │                           │      ├───▶│ row 3  (event 1)    │
 │                           │      └───▶│ row 4  (event 1)    │
 │ event 2 : start=5 count=2 │──────────▶│ row 5  (event 2)    │
 │                           │      └───▶│ row 6  (event 2)    │
 └────────────────────────── ┘           └─────────────────────┘
```

The `momentum` rows of the same TTree point into the `momentum` RNTuple
identically. Each row carries its own `container_name` so the products stay
independent.

---

## 4. The manifest (product registry)

A single JSON sidecar at the strategy root — **one per strategy**, not per
variant, since the product registry is identical across variants. A reader can
answer "does this dataset contain position data, and which variants exist?" by
reading only this file — no ROOT, no opening the `.root`.

```json
{
    "strategy": "strategy_one",
    "root_file": "strategy_one.root",
    "num_events": 10000,
    "total_particles": 69919,
    "variants": [
        { "name": "no-shuffle", "dir": "no-shuffle", "file": "no-shuffle/strategy_one.root" },
        { "name": "shuffle", "dir": "shuffle", "file": "shuffle/strategy_one.root", "shuffle_seed": 7 }
    ],
    "products": [
        {
            "name": "position",
            "product_id": 0,
            "data_container": "position",
            "data_container_type": "RNTuple",
            "index_container": "index",
            "index_container_type": "TTree"
        },
        {
            "name": "momentum",
            "product_id": 1,
            "data_container": "momentum",
            "data_container_type": "RNTuple",
            "index_container": "index",
            "index_container_type": "TTree"
        }
    ]
}
```

| Field                  | Meaning                                                  |
|------------------------|----------------------------------------------------------|
| `strategy`             | which write strategy produced this output                |
| `root_file`            | the ROOT file name inside each variant folder            |
| `num_events`           | events written (the writer's source of truth)            |
| `total_particles`      | sum of all particles written                             |
| `variants[]`           | one entry per write variant                              |
| `.name`                | variant name (`no-shuffle` / `shuffle`)                  |
| `.dir`                 | subfolder under the strategy root holding this variant   |
| `.file`                | data ROOT file, path relative to the strategy root       |
| `.shuffle_seed`        | seed for the permutation (present only on `shuffle`)     |
| `products[]`           | one entry per data product                               |
| `.product_id`          | numeric minor key for the index (`0`=position,`1`=momentum) |
| `.data_container`      | name of the data container inside the ROOT file          |
| `.data_container_type` | storage technology of the data container (`RNTuple`)     |
| `.index_container`     | name of the index container — the shared `index`         |
| `.index_container_type`| storage technology of the index container (`TTree`)      |

A reader opens a variant's data with `<strategy root>/<variant.file>` — the path
is given explicitly, not inferred from the name. `shuffle_seed` appears only on
the `shuffle` variant entry (no shuffle applies to `no-shuffle`). The `_type` fields name the storage technology of each container —
analogous to FORM's *minor technology* in a `Placement`. They let a reader pick
the right API (`RNTupleReader` for the data, a `TTree` for the index) from the
manifest alone, without opening the `.root`.

If a future strategy splits products into separate ROOT files (one per product),
a per-product `file` field can be added to each product entry — reader code stays
the same otherwise.

---

## 5. End-to-end pipeline

```
 Phase 1 (generation)        Phase 2a (fgs_strategy_one)          Phase 2b (fgs_verify)
 ────────────────────        ───────────────────────────          ─────────────────────
 positions.bin  ┐            load_product() each product           read strategy manifest:
 momenta.bin    ├─ in        ──▶ per-event float buffers             products + variants?
 manifest.json  ┘  output/generation         │                      for each variant listed:
                                             ▼                        index lookup (2-key)
                            for each variant: shuffle order,                   │
                            write 2 RNTuples + index TTree,                    ▼
                            BuildIndex(event_id, product_id)       read only a target event's
                                             │                     rows; cross-check values vs
                                             ▼                     the Phase 1 originals (==)
                   strategy_one/<variant>/strategy_one.root                  │
                   strategy_one/manifest.json (one) ─────────────────────────┘
```

---

## 6. Read path (how `fgs_verify` fetches one event)

```
 target_event_id = 42, product_id = 0 (position)
        │
        │ 1. open the "index" TTree (its persistent TTreeIndex is already
        │    in the file — no scan, no map to build)
        ▼
   entry = index->GetEntryNumberWithIndex(42, 0)      ← O(log N) binary search
        │
        │ 2. GetEntry(entry) loads row_start / row_count directly
        ▼
   { row_start = 286, row_count = 2 }   (no-shuffle; differs in shuffle)
        │
        │ 3. open position, read ONLY rows [286, 288)
        ▼
   row 286: event_id=42, x, y, z
   row 287: event_id=42, x, y, z
        │
        │ 4. assert every row's event_id == 42
        │ 5. compare each x/y/z to the original Event  (exact ==)
        ▼
   event 42 PASSED (2 particles)
```

The data RNTuple is never scanned to *find* the event, and the index itself is
not scanned — the persistent two-key `TTreeIndex` does an O(log N) binary search
straight to the `(event_id, product_id)` row. A bounded range read then fetches
exactly the event's rows. `fgs_verify` runs this for **every** variant the
strategy manifest lists, so `no-shuffle` and `shuffle` are both checked against
the same reference data.

### Where the index lives: on disk

The index is **persisted on disk** as the `index` TTree plus its `TTreeIndex`
(built with `BuildIndex("event_id", "product_id")` at write time). There is **no
in-RAM `unordered_map`** in this design: the reader queries the on-disk index
directly with `GetEntryNumberWithIndex`, so nothing is rebuilt when a process
opens the dataset.

> Phase 3 benchmarking note: if the per-lookup TTree cost ever contaminates read
> measurements, load the index into an in-RAM `unordered_map` once at session
> start and measure only data retrieval. That is a benchmarking optimization, not
> part of this layout.

---

## 7. Edge cases the layout handles

| Case | Representation | Behaviour |
|------|----------------|-----------|
| Event with **0 particles** | index row `{id, pid, start, 0}`; data has **no** rows | `row_count = 0`; reader reads nothing, still valid |
| **Missing** event id at read | `GetEntryNumberWithIndex` returns < 0 | lookup fails → `fgs_verify` aborts |
| Dataset smaller than demo id | manifest `num_events` is small | `fgs_verify` skips the `42` target, still tests the last event |
| Gen dir / root file disagree | `events.size() != manifest num_events` | `fgs_verify` aborts with a clear message before reading |
