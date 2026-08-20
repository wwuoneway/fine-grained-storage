# strategy_one RNTuple layout

`strategy_one` reads the FGS2 binary dataset ([`FORMAT.md`](FORMAT.md)) and re-encodes
it with ROOT's **RNTuple** columnar format. This document is the authoritative schema
for that output and must stay in agreement with
[`writing/strategy_one/main.cpp`](../writing/strategy_one/main.cpp).

Producer: `fgs_strategy_one`. Consumers: `EventReader`
([`reading/event_reader.cpp`](../reading/event_reader.cpp)), which the read benchmarks
drive, and `fgs_verify`, which cross-checks the output against the Phase 1 binaries.

## Terms

| Term | Here | FORM counterpart |
|---|---|---|
| product | one physics quantity per particle: `position` or `momentum` | data product |
| container | one named storage object inside the ROOT file | Container |
| token | `{container, entry}`, locating one product of one event | Token |
| variant | one write of the same events in a different physical row order | -- |

## 1. Output tree

One directory per write variant under the configured `output_root`, plus one manifest
for the whole strategy at its root. The product registry does not change across
variants, so it is not duplicated per variant.

```
<output_root>/
  manifest.json                 product registry + per-variant statistics
  no-shuffle/
    strategy_one.root           2 data RNTuples + 1 index TTree
  shuffle/
    strategy_one_shuffled.root  same schema, different row order
```

Each `.root` file holds three containers: `position_container` and `momentum_container`
(RNTuple, section 2) and `index` (TTree, section 3). `manifest.json` records which
products exist, which container and technology each uses, and per variant the file path,
its size, and its cluster and page counts, so a reader can answer "does this dataset
have momentum data" without opening ROOT.

### Write variants

The `variants` list in the writing config selects which are produced; the study configs
request `no-shuffle` only.

| Variant | Row order | File |
|---|---|---|
| `no-shuffle` | event `e` occupies row `e` | `strategy_one.root` |
| `shuffle` | a permutation seeded by `shuffle_seed`, the same for both products | `strategy_one_shuffled.root` |

The axis exists because DUNE events do not arrive sorted. A shuffle changes only the
physical row order: each event remains one self-contained row, and the index maps
`event_id` to that row either way, so both variants return identical values to a reader.

## 2. Data containers

`position_container` and `momentum_container` have the same shape and differ only in
element type and field name. One row is one whole event.

| Container | Field | Type |
|---|---|---|
| `position_container` | `event_id` | `uint64` |
| | `vec_particles_pos` | `vector<Position>`, `Position = {x, y, z : float}` |
| `momentum_container` | `event_id` | `uint64` |
| | `vec_particles_mom` | `vector<Momentum>`, `Momentum = {px, py, pz : float}` |

`Position` and `Momentum` are defined in `core/include/fgs/types.hpp`.

An event with `N` particles is one row whose vector field holds `N` elements. An event
with zero particles is still one row, with an empty vector. `event_id` is stored on
every row so that a row is self-describing and `fgs_verify` can assert the row it read
belongs to the event it asked for, independently of the index.

RNTuple does not store these rows contiguously: a vector field is split into an offsets
column plus one column per scalar member, so a reader that wants only `x` reads only
that column. See ROOT's
[binary format specification](https://github.com/root-project/root/blob/master/tree/ntuple/doc/BinaryFormatSpecification.md).

## 3. Index container

A single TTree named `index` maps each `event_id` to the per-product tokens that locate
that event's row in each data container.

| Branch | Type | Meaning |
|---|---|---|
| `event_id` | `uint64` | which event |
| `index_value` | `fgs::EventIndex`, i.e. `map<string, Token>` | product name -> token |

`Token = {container : string, entry : uint64}` (`core/include/fgs/token.hpp`).
`container` names the data RNTuple and `entry` is the row within it. One index row holds
one entry per product, so both products of an event are located from a single row. Three
events, `no-shuffle`:

| row | `event_id` | `index_value` |
|---|---|---|
| 0 | 0 | `{position: {position_container, 0}, momentum: {momentum_container, 0}}` |
| 1 | 1 | `{position: {position_container, 1}, momentum: {momentum_container, 1}}` |
| 2 | 2 | `{position: {position_container, 2}, momentum: {momentum_container, 2}}` |

Under `shuffle`, index rows are filled in write order, so both the index rows and the
`entry` values follow the permutation. The container name never changes.

```
   index row (event 1)                   position_container
 +-----------------------------+         +----------------------------+
 | event_id = 1                |         | row 0  (event 0)           |
 | index_value["position"]     |         | row 1  (event 1) <---------+
 |   = {position_container, 1} |-------->| row 2  (event 2)           |
 | index_value["momentum"]     |         +----------------------------+
 |   = {momentum_container, 1} |----+     momentum_container
 +-----------------------------+    |    +----------------------------+
                                    +--->| row 1  (event 1)           |
                                         +----------------------------+
```

The index is a TTree rather than an RNTuple because TTree supports a persistent
value-based index, which RNTuple does not. After the rows are filled the writer calls
`BuildIndex("event_id")`, storing a `TTreeIndex` in the file, so a reader may resolve an
event with `GetEntryNumberWithIndex` in O(log N) and no forward scan. `EventReader` does
not use it: it scans the branch once at construction and answers later lookups from
memory. Both are lookups against the same on-disk index.

## 4. Invariants and edge cases

The writer guarantees two properties of every file:

- the `N` event ids are exactly `0` to `N-1`, each appearing once,
- a product always lives in the same container across all events of a file.

`EventReader` counts the distinct ids while scanning the index and throws at
construction if any id below that count is missing. It does not verify the second
property: it takes each product's container from the first index row naming that
product and reuses it for every event.

| Case | Representation | Behaviour |
|---|---|---|
| Event with 0 particles | one row whose vector field is empty | `vec.size() == 0`; the row is read and returns 0 elements |
| Missing event id at read | id outside `[0, N)` | `EventReader` throws; a gap inside the range is rejected when the index is scanned, not mid-run |
| Product not in the file | product name absent from the index | requesting it throws at construction, listing the products the file does hold |
| Dataset smaller than a requested id | manifest `total_events` is small | `fgs_verify` validates ids are in range before reading |
| Generation dir and ROOT file disagree | `events.size() != total_events` | `fgs_verify` aborts before reading |