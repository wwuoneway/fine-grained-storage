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
    strategy_one_shuffled.root
```

A "data product" here is one physics quantity per particle. There are two:
`position` and `momentum`. Each product gets one **data** RNTuple
(`position_container`, `momentum_container`). Both share a single combined
**index**, a TTree named `index` (not an RNTuple).

```
        strategy_one.root  (TFile)
        +------------------------------------------------------------+
        | position_container  momentum_container  RNTuples (numbers) |
        | index               TTree               event -> tokens    |
        +------------------------------------------------------------+
                              ^
                              |  described by
              manifest.json --+  (which products exist, where they live)
```

### Write variants (the ordering axis)

The variants exist to study how on-disk event order affects reads, since in DUNE
events do not arrive sorted.

- **`no-shuffle`** -- events written in event order; event `e` lands in row `e`
  of each container.
- **`shuffle`** -- events written in a permutation produced by a seeded shuffle
  (`shuffle_seed` in the config). The same permutation is used for both products.

The shuffle changes **only** the physical row order. Each event is still a single
self-contained row, and the index maps `event_id` to that row regardless of order,
so a reader gets identical results from either variant.

---

## 2. Data RNTuples -- one row per event

`position_container` and `momentum_container` have the same shape; only the
element type and field names differ. Each **row is one whole event**: an
`event_id` plus the event's entire particle collection stored as a single vector
field.

```
position_container                        momentum_container
 fields:                                   fields:
   event_id           : uint64              event_id           : uint64
   vec_particles_pos  : vector<Position>    vec_particles_mom  : vector<Momentum>

 Position = { x, y, z : float }           Momentum = { px, py, pz : float }
 1 row  =  1 event                        1 row  =  1 event
```

An event with `N` particles is one row whose vector field holds `N` elements. An
event with **zero** particles is still one row, with an empty vector.

`event_id` is stored on every row on purpose: it makes each row self-describing
and lets `fgs_verify` assert that the row it read really belongs to the event it
asked for, independently of the index.

### Worked example (`no-shuffle`)

Three events with particle counts 2, 3, 2. The `position_container` holds:

```
position_container  (logical row view)
 row │ event_id │ vec_particles_pos
 ────┼──────────┼────────────────────────────────────────────────────
  0  │    0     │ [ (12.31,-5.63,301.20), (-64.39,53.56,-322.52) ]        event 0 (2)
  1  │    1     │ [ (8.10,19.44,-77.01), (-12.55,-3.20,140.06),
     │          │   (64.77,41.13,-9.88) ]                                 event 1 (3)
  2  │    2     │ [ (21.76,-57.48,-302.83), (-6.15,26.12,139.40) ]        event 2 (2)
```

In `shuffle` the same three rows are written in a shuffled order (say event 1,
then 2, then 0), so event 0 would land in row 2 instead of row 0. Each event's
row is unchanged; only its row number differs, and the index records it.

### Columnar, not row-major

RNTuple does not store the table row-by-row. A `vector<Position>` field is split
into an offsets column (where each event's particles start) plus one column per
scalar member. Logically it is the table above; physically it is:

```
 event_id stream:            [ 0, 1, 2 ]
 vec offsets stream:         [ 2, 5, 7 ]        (cumulative particle counts)
 vec_particles_pos.x stream: [ 12.31, -64.39, 8.10, -12.55, 64.77, 21.76, -6.15 ]
 vec_particles_pos.y stream: [ -5.63, 53.56, 19.44, -3.20, 41.13, -57.48, 26.12 ]
 vec_particles_pos.z stream: [ 301.20, -322.52, -77.01, 140.06, -9.88, -302.83, 139.40 ]
```

A reader that only wants `x` reads only that member's stream.

---

## 3. Index TTree -- one row per event (the address book)

A single TTree named `index` maps each `event_id` to the per-product **tokens**
that locate that event's row in each data RNTuple. It is a **TTree**, not an
RNTuple, because TTree provides a persistent value-based index (`BuildIndex`) for
O(log N) lookup without scanning; RNTuple has no such index.

```
index TTree
 branches:
   event_id     : uint64                          which event
   index_value  : map<string, Token>              product name -> Token

 Token = { container : string, entry : uint64 }
 1 row  =  1 event      (one map holding both products' tokens)
```

`index_value` is `fgs::EventIndex`, the `core` type `map<string, Token>`
(`core/include/fgs/token.hpp`). For each event it holds one entry per product:
`"position" -> {container, entry}`
and `"momentum" -> {container, entry}`, where `container` names the data RNTuple
and `entry` is the row within it. Because each event is a single row, `entry` is
just that event's row number in the container.

For the worked example above (3 events, `no-shuffle`), the `index` TTree is:

```
index TTree
 row │ event_id │ index_value
 ────┼──────────┼──────────────────────────────────────────────────────────────
  0  │    0     │ { position:{position_container,0}, momentum:{momentum_container,0} }
  1  │    1     │ { position:{position_container,1}, momentum:{momentum_container,1} }
  2  │    2     │ { position:{position_container,2}, momentum:{momentum_container,2} }
```

In `shuffle`, if event 0 is written last, its row (and both its token `entry`
values) become `2` instead of `0`. The container name never changes.

After all rows are filled, the writer calls `index_tree->BuildIndex("event_id")`,
which stores a persistent single-key `TTreeIndex` inside the file, so a reader can
jump to an event's row with `GetEntryNumberWithIndex` in O(log N) with no forward
scan. `fgs_verify` instead reads the whole index into a RAM map once at startup
(see section 6); both approaches are lookups against the same on-disk index.

> Note on `shuffle`: the index rows are filled in write order, so in the shuffled
> variant the *physical* index rows are also shuffled. `BuildIndex` makes lookup
> value-based, so order does not matter to a reader.

### How the index points into the data

```
   index TTree (event 1)                 position_container
 +-----------------------------+         +----------------------------+
 | event_id = 1                |         | row 0  (event 0)           |
 | index_value["position"]     |         | row 1  (event 1) <---------+
 |   = {position_container, 1} |--------▶| row 2  (event 2)           |
 | index_value["momentum"]     |         +----------------------------+
 |   = {momentum_container, 1} |----+     momentum_container
 +-----------------------------+    |    +----------------------------+
                                    +---▶| row 1  (event 1)           |
                                         +----------------------------+
```

One index row carries both products' tokens, and each token names its own
container, so the products stay independent.

---

## 4. The manifest (product registry)

A single JSON sidecar at the strategy root, **one per strategy**, not per variant,
since the product registry is identical across variants. A reader can answer "does
this dataset contain position data, and which variants exist?" by reading only
this file: no ROOT, no opening the `.root`.

```json
{
    "generated_at": "2026-07-27 09:41:12 UTC",
    "strategy": "strategy_one",
    "total_events": 10000,
    "avg_particles_per_event": 6.9919,
    "avg_event_size_mib": 0.001,
    "total_particles": 69919,
    "variants": [
        {
            "name": "no-shuffle",
            "dir": "no-shuffle",
            "file": "no-shuffle/strategy_one.root",
            "file_mib": 3.573,
            "containers": {
                "position_container": { "clusters": 1, "pages": 8 },
                "momentum_container": { "clusters": 1, "pages": 8 }
            }
        },
        {
            "name": "shuffle",
            "dir": "shuffle",
            "file": "shuffle/strategy_one_shuffled.root",
            "file_mib": 3.674,
            "shuffle_seed": 7,
            "containers": {
                "position_container": { "clusters": 1, "pages": 8 },
                "momentum_container": { "clusters": 1, "pages": 8 }
            }
        }
    ],
    "products": [
        {
            "name": "position",
            "container": "position_container",
            "container_type": "RNTuple",
            "index_container": "index",
            "index_container_type": "TTree"
        },
        {
            "name": "momentum",
            "container": "momentum_container",
            "container_type": "RNTuple",
            "index_container": "index",
            "index_container_type": "TTree"
        }
    ]
}
```

| Field                    | Meaning                                                    |
|--------------------------|------------------------------------------------------------|
| `generated_at`           | UTC timestamp the manifest was written                     |
| `strategy`               | which write strategy produced this output                  |
| `total_events`           | events written (the writer's source of truth)              |
| `avg_particles_per_event`| mean particle count per event                              |
| `avg_event_size_mib`     | mean on-disk footprint of one event (averaged over variants)|
| `total_particles`        | sum of all particles written                               |
| `variants[]`             | one entry per write variant                                |
| `.name` / `.dir`         | variant name and its subfolder under the strategy root     |
| `.file`                  | data ROOT file, path relative to the strategy root         |
| `.file_mib`              | on-disk size of that variant's ROOT file, in MiB           |
| `.shuffle_seed`          | seed for the permutation (present only on `shuffle`)       |
| `.containers{}`          | per data container, its RNTuple `clusters` and `pages`     |
| `products[]`             | one entry per data product                                 |
| `.container`             | name of the data RNTuple inside the ROOT file              |
| `.container_type`        | storage technology of the data container (`RNTuple`)       |
| `.index_container`       | name of the index container, the shared `index`            |
| `.index_container_type`  | storage technology of the index container (`TTree`)        |

A reader opens a variant's data with `<strategy root>/<variant.file>`; the path is
given explicitly, not inferred from the name. The `_type` fields name the storage
technology of each container, analogous to FORM's *minor technology* in a
`Placement`. They let a reader pick the right API (`RNTupleReader` for the data, a
`TTree` for the index) from the manifest alone, without opening the `.root`.

If a future strategy splits products into separate ROOT files (one per product), a
per-product `file` field can be added to each product entry; reader code stays the
same otherwise.

---

## 5. End-to-end pipeline

```
 Phase 1 (generation)        Phase 2a (fgs_strategy_one)          Phase 2b (fgs_verify)
 --------------------        ---------------------------          ---------------------
 positions.bin  ┐            load_product() each product          read strategy manifest:
 momenta.bin    ├─ in        --> per-event float buffers            products + variants?
 manifest.json  ┘  output/generation         │                     for each variant listed:
                                             ▼                        index lookup by event_id
                            for each variant: shuffle order,                  │
                            write 2 RNTuples + index TTree,                   ▼
                            BuildIndex(event_id)                  read the target event's row;
                                             │                    cross-check values vs the
                                             ▼                    Phase 1 originals (==)
                   strategy_one/<variant>/<root file>                       │
                   strategy_one/manifest.json (one) ────────────────────────┘
```

---

## 6. Read path (how `fgs_verify` fetches one event)

```
 target_event_id = 42, product = "position"
        │
        │ 1. read the whole "index" TTree once into
        │    unordered_map<event_id, EventIndex>
        ▼
   idx = tokens[42]                              (the event's per-product tokens)
        │
        │ 2. token = idx["position"] = { container="position_container", entry=42 }
        ▼
   { container = "position_container", entry = 42 }   (no-shuffle; entry differs in shuffle)
        │
        │ 3. open position_container, read ONLY row `entry`
        ▼
   row 42: event_id = 42, vec_particles_pos = [ (x,y,z), ... ]
        │
        │ 4. assert the row's event_id == 42
        │ 5. compare each Position to the original Event  (exact ==)
        ▼
   event 42 PASSED (2 particles)
```

The data RNTuple is never scanned to *find* the event: the token gives its exact
row, and a whole-row read fetches the event's entire particle vector at once.
`fgs_verify` runs this for **every** variant the strategy manifest lists, so
`no-shuffle` and `shuffle` are both checked against the same reference data.

### Where the index lives: on disk

The index is **persisted on disk** as the `index` TTree plus its `TTreeIndex`
(built with `BuildIndex("event_id")` at write time). `fgs_verify` reads the whole
index into an in-RAM `unordered_map` once at startup and then looks tokens up from
that map, so the per-event read path touches only the data containers.

> Phase 3 benchmarking note: whether to keep the index in RAM (as `fgs_verify`
> does) or query the on-disk `TTreeIndex` per lookup with `GetEntryNumberWithIndex`
> is a benchmarking choice, not part of this layout.

---

## 7. Edge cases the layout handles

| Case | Representation | Behaviour |
|------|----------------|-----------|
| Event with **0 particles** | one row whose vector field is empty | `vec.size() == 0`; reader reads the row, compares nothing, still valid |
| **Missing** event id at read | `event_id` absent from the index map | lookup fails, `fgs_verify` aborts with a clear message |
| Dataset smaller than a demo id | manifest `total_events` is small | `fgs_verify` validates requested ids are in range before reading |
| Gen dir / root file disagree | `events.size() != manifest total_events` | `fgs_verify` aborts before reading |
