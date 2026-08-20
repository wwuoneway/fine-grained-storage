# Google Summer of Code 2026 - Final Work Report
## Fine grained storage for the DUNE experiment


<img width="1434" height="413" alt="image" src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/refs/heads/main/gsoc_cern_hsf.png" />



## Details

| | |
|---|---|
| **Contributor** | [Ahmed Idani](https://github.com/Ahmed-Idani) |
| **Organization** | [CERN-HSF](https://hepsoftwarefoundation.org/activities/gsoc.html) |
| **Mentors** | [Wanwei Wu](https://www.anl.gov/profile/wanwei-wu), [Peter Van Gemmeren](https://www.anl.gov/profile/peter-van-gemmeren) |
| **Project proposal** | [Fine grained storage for the DUNE experiment](https://hepsoftwarefoundation.org/gsoc/2026/proposal_DUNE_FORM.html) |
| **Project repository** | [https://github.com/wwuoneway/fine-grained-storage](https://github.com/wwuoneway/fine-grained-storage) |


## Overview

**[DUNE](https://www.dunescience.org/)**, the neutrino experiment, relies on computing systems built to handle PB-scale datasets and GB-scale event objects, accommodate diverse workflows and provide flexible access to large detector data objects. Thus, traditional event-centric processing models become increasingly limiting for DUNE-scale workflows.

To address this, the experiment is developing a new data processing framework called **[Phlex](https://github.com/Framework-R-D/phlex)** that will work with fine grained data. It's I/O layer FORM will support eager writing of sub-records into individual containers without synchronization. However when reading data, clients may need to collect different sub-records at the same time with synchronization.

This takes us to a narrower question underneath it: on ROOT's RNTuple, the storage format DUNE is moving to, how expensive is that independence, and where does the cost actually go?

This project aims to answer this question empirically by implementing a standalone benchmark and study on ROOT's RNTuple, ROOT being the only dependency of the benchmark (not Phlex)

---


# 1. Background: why this benchmark, and how it maps to DUNE
## 1.1 The problem in DUNE's terms

A DUNE event, in Phlex, is not one flat record. Its separate data products (a reconstructed track, Hit, ...) can be written to separate FORM Containers by different pipeline stages, at different times, with no synchronization between them. Reading a specific entry back means resolving a Token, FORM's internal read-time address (technology, file, container, and row).


Writing is easy. Reading is a bit tricky. When we write to a container like Hits, the write order is not clear, but if we only need to read Hits, we could say read them in the order they were written.
The complication comes in when we write more than one data product, parallel programs will be writing to Hit and Track containers as an example and without synchronization not only is the order not determined but also they may be slightly in different orders.
A reader that needs two containers for the same event can keep one of them in its cheap, natural order, but the other is then necessarily read out of its own order and that out-of-order read is the cost this benchmark measures.

## 1.2 What this benchmark actually builds

This suite doesn't implement FORM, it builds the smallest thing that has the same core problem: each data product stored in its own container, laid out in its own order on disk, so an event's products are reached only through an index at read time. To do so, we needed some concepts and abstractions from FORM to map to.

| FORM concept | This benchmark |
|---|---|
| Container | one RNTuple per data product sharing the same .root file|
| Technology | RNTuple (the format under study) |
| Index | the `index` which serves as a lookup table in disk as a TTree, mapping `event_id` to each product's Token |
| Token | `{container, entry}`, naming the row a product's data lives at |
| data product | for our study: `position`, `momentum` |

section 2 gives the concrete schema

# 2. Proposed method

This benchmark runs in three phases, all driven from one configuration file: generate synthetic data, write it to RNTuple, then read it back while collecting performance metrics and plot the results.

<p align="center">
  <img width="560" alt="phases" src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/refs/heads/main/phases.png" />
</p>

## 2.1 Phase 1: synthetic data generation
An event is one simulated collision holding `N` particles, and `N` changes from one event to the next. A particle is described by two things, where it is and where it is going, and each is a struct of three `float`s:

```cpp
struct Position { float x, y, z; };
struct Momentum { float px, py, pz; };

struct Event {
  std::uint64_t id;
  std::vector<Position> positions; // size N
  std::vector<Momentum> momenta;   // size N (parallel to positions)
};
```

So an `Event` is not a flat record: it carries two vectors of the same length, and particle `i`
of that event is the pair `(positions[i], momenta[i])`. **`position` and `momentum` are the two
data products** of this benchmark, and phase 2 stores each of them in its own container, which
is what makes an event's data reachable only through an index at read time.

| Field | Type | How it is drawn |
|---|---|---|
| `N`, particles in the event | `uint32` | uniform integer in `[particles.min, particles.max]`, drawn once per event |
| each `Position` | 3 x `float` | each axis uniform in its own configured range |
| each `Momentum` | 3 x `float` | each component Gaussian on `mean` and `stddev`, redrawn while the magnitude is below `min_magnitude` |

The knobs that pick a dataset:

| Knob | What it sets 
|---|---|
| `num_events` | how many events the dataset holds 
| `particles.min` / `particles.max` | the range `N` is drawn from 
| `seed` | seeds the single `mt19937_64` behind every draw above, so a config reproduces a dataset exactly


The output of this phase is two binary files, **positions.bin** and **momenta.bin**, written in a format defined for this benchmark and specified in [docs/FORMAT.md](https://github.com/wwuoneway/fine-grained-storage/blob/main/docs/FORMAT.md), plus a manifest.json recording the configuration used to generate them. These files are the input to the next phase.

## 2.2 Phase 2: writing to RNTuple
The writer reads the binary files and re-encodes them into ROOT's RNTuple format, one RNTuple per data product.
Data can be written in two variants: `no-shuffle` (the same order as the binary files) and `shuffle` (a pseudorandom permutation of the rows, seeded by `shuffle_seed`). The output is one `.root` file per variant, plus a manifest.json recording the necessary metadata needed at lookup.

Each container is declared as two fields, and one row is one whole event. RNTuple does not store that row as a row, it splits every field into its own column:

| Container | What we declare | What RNTuple stores on disk |
|---|---|---|
| `position_container` | `event_id` : `uint64` | one `uint64` column |
| | `vec_particles_pos` : `vector<Position>` | an offsets column saying where each event's particles start, plus one float column per member: `x`, `y`, `z` |
| `momentum_container` | `event_id` : `uint64` | one `uint64` column |
| | `vec_particles_mom` : `vector<Momentum>` | the same, with `px`, `py`, `pz` |


To be able to locate the events, an index is needed. For this, we went with a ROOT's TTree to have persistent storage which comes with BuildIndex for O(log N) value-based lookup. The index maps each `event_id` to the per-product tokens that locate that event's row in each data container.

more on the schema in [docs/RNTUPLE_LAYOUT.md](https://github.com/wwuoneway/fine-grained-storage/blob/main/docs/RNTUPLE_LAYOUT.md)

## 2.3 Phase 3: reading and measurements
The `EventReader` opens the `.root` file and scans the index TTree once at startup, keeping in memory only the row number of each product of each event. It also opens one RNTuple reader per container up front. This is deliberate, the timed loop should pay for reading data and nothing else.

<details>
<summary>How the in-memory index is laid out</summary>

> Event ids are dense, they run from `0` to `N-1` with no holes, so the index does not need a hash map. The products are sorted once, which gives each of them a position `p`, and every row number goes into one flat array:
>
> ```
> entries_[event_id * n_products + p]
> ```
>
> With two products (`momentum`, `position`) and three events, the array is just:
>
> ```
> [ m0, p0, m1, p1, m2, p2 ]
> ```
>
> so the row holding `position` for event 1 is `entries_[1 * 2 + 1]`. A lookup is a multiply, an add and a load, and the array costs 8 bytes per product per event.
>
> The container is not stored per event. A product always lives in the same container, so the reader opens one container per product and the same `p` selects both the row number and the container to read it from.

</details>
</br>

Reading one product of one event is then two steps:
1) look up the row in memory O(1)
2) one `LoadEntry` on that row, which brings back the event's whole particle vector

The read benchmark drives this reader over a set of cases. A case fixes what is read, the file, the products and the event count, and then varies the knobs. Those fall into two groups: the ones ROOT exposes on its read API, and the ones we control from outside it.

**What ROOT gives us** (`RNTupleReadOptions`)

| Option | Values | What it does |
|---|---|---|
| `cluster_cache` | `off`, `on` | ROOT's cluster read-ahead. Off means pages are fetched on demand |
| `implicit_mt` | `off`, `default` | ROOT's implicit multithreading, which decompresses pages in parallel |
| metrics (passive) | always on | turns on ROOT's per-container counters, which is where our numbers come from |

**What we control ourselves**

| Knob | Values | What it does |
|---|---|---|
| `access_pattern` | `sequential`, `scatter`, `random` | the order event ids are visited. All three visit every event exactly once, so only the order changes |
| `scatter_distance` | integer | how far an id may move from its sequential position, `scatter` only |
| `products` | `position`, `momentum`, or both | which containers the run actually touches |
| `num_events` | integer | how many events the run reads |
| `os_cache` | `cold`, `warm` | whether the file is in the OS page cache when the run starts or we evict the cache |
| `repetitions` | integer | how many times the case is rerun |

<details>
<summary>How the scatter order is built, the algorithm</summary>

> We start from the sequential order and walk it once, swapping each position with another one no further than `distance` away:
>
> ```
> order = [0, 1, 2, ..., n-1]
> if distance == 0: return order            # identity, our sequential baseline
>
> for i in 0 .. n-1:
>     j = i + random(-distance, +distance)   # a partner at most `distance` away
>     if j < 0:    j = 0                     # but keep it inside the array
>     if j > n-1:  j = n-1
>     swap(order[i], order[j])
> ```
>
> Because it only ever swaps, the result is still a permutation. Every event is read exactly once, so a scatter case does the same amount of work as a sequential or a random one and only the order differs.
>
> `distance` is the knob. At 0 the order is untouched and we get sequential back, and as it grows the order loosens toward random. That is the point of this pattern, it lets us walk the space between the two extremes instead of only measuring the endpoints.

</details>

<details>
<summary>Cache eviction technique</summary>

> Cold has to mean the file is not in the OS page cache when the run starts. Before each repetition we open every container file and call `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` on it, which asks the kernel to drop that file's pages.
>
> The catch is that `POSIX_FADV_DONTNEED` only drops **clean** pages. A page that is still dirty, written but not yet flushed to disk, is kept. A file we had just written in phase 2 would therefore stay partly cached and the run would not be cold at all. That is why the runner calls `sync` before each benchmark: it flushes the dirty pages out, they become clean, and only then can fadvise drop them.
>
> We use this rather than `drop_caches` because it needs no root and it evicts only our own files instead of the whole machine's cache.
>
> Warm is the opposite and has to be explicit: a warm case runs an untimed warmup pass first. Ambient cache state is rejected as a condition, since we would have no idea what is in it.

</details>
</br>
Each case reports wall time plus ROOT's own counters:

- time spent in storage I/O, and time spent decompressing
- bytes pulled from storage, and bytes returned after decompression
- pages fetched, pages actually decompressed, and clusters loaded
- the ratios derived from those: decompressed over wanted bytes, and pages unzipped over pages
  in the file

Those counters are what let us attribute the cost instead of only measuring it. The output is a `summary.csv` with one row per case, and the plots are generated from it.

## 2.4 From one file to N benchmark cases 
The benchmark uses one axes file as the single source of truth ([study-1.json](https://github.com/wwuoneway/fine-grained-storage/blob/main/configs/study/shared/study-1.json)). It holds four blocks:

| Block | What it holds |
|---|---|
| `defaults` | generation settings shared by every dataset, the seed and the position and momentum distributions |
| `generation` | one entry per dataset: how many events, and how many particles per event |
| `writing` | which variants to write, the shuffle seed, and the page sizes to try |
| `reading` | the read axes of 2.3: access pattern, scatter distance, products, cache state, cluster cache, implicit MT, repetitions |

A generator expands that one file into the concrete configs each phase actually consumes: one generation config per dataset, then one writing config and one set of benchmark cases per (dataset, page size) pair. They are regenerated on every run and never edited by hand, so the axes file stays the only thing anyone has to change.

The easiest way to read that file is as a matrix: every list in it is one axis, and one benchmark case is one cell, a single pick from each axis.

Take the file linked above. It asks for 7 datasets, written one way (`no-shuffle`, one page size), and read with 2 access patterns where `scatter` carries 9 distances, one cache state, one cluster cache setting and one implicit MT setting. That comes out as 1 sequential case plus 9 scatter cases for each dataset, so 7 x 10 = **70 benchmark cases**, each one a row in `summary.csv`.

The runner script then walks the datasets and, for each one, generates, writes and benchmarks. Generation and writing are skipped when nothing upstream of them changed, so running the study a second time repeats the measurements without rebuilding the data. Plotting runs between benchmarks, never during one, so it cannot perturb a measurement.

## 2.5 Measurement environment

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-13620H, 10 physical cores (6 performance + 4 efficiency), 16 logical |
| Memory | 2 x 8 GB DDR5, one module per memory controller, 15.2 GiB available to the OS |
| Storage | WD PC SN5000S NVMe SSD, 476.9 GiB, link 16.0 GT/s x4, `ext4` |
| OS | Ubuntu 24.04.4 LTS, kernel Linux 7.0.0-28-generic x86_64 |
| ROOT | 6.40.02 |


<details>
<summary>What was held fixed during a run</summary>

> | Condition | Setting | Why |
> |---|---|---|
> | CPU pinning | the read benchmark runs under `taskset -c <cpus to pin>` | on a hybrid CPU an unpinned case can land on a performance core in one repetition and an efficiency core in the next, an effect larger than most knobs under test |
> | Core isolation | those same CPUs isolated at boot with `isolcpus=` | keeps unrelated processes off the measured cores |
> | Hyper-Threading | one CPU per physical core, never a sibling pair | siblings share caches and execution units, so the reader and ROOT's I/O thread would contend |
> | CPU frequency | `performance` governor | `powersave` lets the clock drift between repetitions |
> | ASLR | off | same heap and stack layout every repetition, so cache and alignment effects stop moving between runs |
> | Page cache | dropped once before the sweep, then per repetition as described in 2.3 | |
>
> The isolated CPUs go in `FGS_BENCH_CPUS` and the rest of the procedure is in [BEFORE_RUN.md](https://github.com/wwuoneway/fine-grained-storage/blob/main/BEFORE_RUN.md). On another machine the CPU list changes, nothing else does.

</details>

</br>

# 3. Study and experiments

As we said in section 1, a reader collecting one event out of several containers has to give
up the physical order of all but one of them. This section measures what that costs. 3.1 and
3.2 set up the two things the results rest on, why a reader ends up out of order and what a
read costs in RNTuple. Two studies follow: the first asks where the wall time of an
out-of-order read goes, the second asks what sets the size of the part that grows.

## 3.1 Why a reader ends up out of order

In Phlex, different pipeline stages write different data products into different FORM
Containers, at different times and without synchronising with each other. Nothing keeps two
containers in step, so the row an event occupies in one has no relation to the row it occupies
in another: event 5 can be row 5 in `position` and row 900 in `momentum`.

A reader that needs both products of that event can follow one container in its natural order
cheaply. The other is then necessarily visited out of its own physical order. That is what
this benchmark isolates. The question is not what it costs to read two products, it is what it
costs to read one product out of its own storage order.

Neither the unsynchronised writing nor a shuffled file is reproduced here. Every file in
section 3 is written in event order (`no-shuffle`), and the mismatch is simulated on the read
side, by asking the reader to visit events in a scattered order. It is the same experiment
seen from the other end: a reader walking a scattered file in event order and a reader walking
an ordered file in a scattered order touch the same pages in the same sequence, so they pay
the same. The read side is just the easier one to control: the visit order is a seeded list
that costs nothing to change between cases, while concurrent writers would leave a different
layout on every run and need a fresh file for every case.

`scatter_distance` is the control on that drift. It bounds how far the reader may stray from
the physical order, so instead of two endpoints, fully sequential and fully random, the cost
comes out as a curve with a known amount of disorder behind every point. The swap algorithm is
in 2.3.

## 3.2 What a read actually costs: pages

A column is not a loose array of values on disk. RNTuple cuts each column into pages, each
page holding a contiguous block of rows, and compresses each page as a single unit
(see the [layout document](https://github.com/wwuoneway/fine-grained-storage/blob/main/docs/RNTUPLE_LAYOUT.md)
of 2.2 and ROOT's binary format specification linked from it).

Three consequences follow, and the whole of section 3 rests on them:

1. To hand back a given row, RNTuple finds the page holding it and decompresses that whole
   page. One row cannot be decompressed on its own.
2. Reading in order drains a page before moving on, so one decompression serves every row in
   it.
3. A scattered order can leave a page after a single row, come back later, and pay the full
   decompression again.

So the penalty is not caused by the offsets being scattered. It is caused by how many useful
rows one decompression yields, and that is set by how many events fit into a page.

### How many events fit into a page

Events per page is a property of a column, not of a file, so it is worth reading it off a real
dataset. A small ROOT macro in the repo prints ROOT's own per-column page counts:

```console
$ root -l -b -q 'scripts/inspect_columns.C("momentum_container", "output/writing/study/s409600_p256-256/default/no-shuffle/strategy_one.root")'

col  0  field=event_id                  pages=8    elements=409600      compressed=5841 B
col  1  field=vec_particles_mom         pages=8    elements=409600      compressed=404 B
col  2  field=vec_particles_mom._0.px   pages=404  elements=104857600   compressed=355309121 B
col  3  field=vec_particles_mom._0.py   pages=404  elements=104857600   compressed=355315953 B
col  4  field=vec_particles_mom._0.pz   pages=404  elements=104857600   compressed=355310952 B
```

This dataset is 409,600 events of 256 particles each. The `px` column holds one float per
particle, 104,857,600 of them, cut into 404 pages: about 259,548 elements per page, which is
about 1,014 events per page. The `event_id` column holds one value per event and covers the
same 409,600 rows in 8 pages, so on that column one page covers 51,200 events.

The three payload columns carry essentially all of the compressed bytes, 1.07 GB against 6 kB
in the other two, so they are what a reader pays for, and "events per page" below always means
events per payload page. Two page counts appear in this report and they are not the same
number: the dataset tables report pages in the widest single column (404 here), while the
figures annotate pages across all five columns of the container (404 x 3 + 8 + 8 = 1,228).

## 3.3 Study one: where the wall time goes

The question: at a fixed payload, when a reader gives up the physical order, does it pay in
storage time or in CPU time? The expectation from 3.2 is CPU time, since a revisited page is
re-decompressed but is likely to still be in the operating system's cache.

The four datasets hold the same 100 million particles cut into different numbers of events, so
the raw payload is identical across them and only the events per page changes. The cluster
cache is off here on purpose: with it on, ROOT reads ahead on a background thread,
`timeWallRead` stops being time on the critical path, and the split this study is after stops
meaning anything.

### Dataset used

| dataset | events | particles per event | raw payload per event | events per page | pages in the widest column |
|---|---:|---:|---:|---:|---:|
| `s400_p262144-262144` | 400 | 262,144 | 6 MiB | 1.00 | 400 |
| `s1600_p65536-65536` | 1,600 | 65,536 | 1.5 MiB | 3.96 | 404 |
| `s102400_p1024-1024` | 102,400 | 1,024 | 0.023 MiB | 253 | 405 |
| `s1638400_p64-64` | 1,638,400 | 64 | 0.001 MiB | 4,045 | 405 |

All four hold the same 100 million particles, written with the same settings. The `momentum`
product the reader touches is 12 bytes per particle, so 1,200 MiB of raw payload, which sits
in about 1,016 MiB of compressed pages on disk. The per-event column above counts both
products, at 24 bytes per particle. Only the number of events that payload is cut into
changes, and with it the number of events that share a single 1 MiB page.

The last column counts pages in the column that has the most of them, one of the three
momentum floats. Those three are where the bytes are (3.2), while `event_id` and the offsets
column need 8 pages each for the whole dataset, so a page of the widest column is what the
reader actually pays to decompress. Events per page is that column's events divided by its
pages, and it is the number every result in this section turns on.

The first row is the control: with 400 pages carrying 400 events, one page holds one event, so
no visit order can make the reader touch a page twice.

### Read and write options

| Setting | Study one |
|---|---|
| write options | ROOT's defaults, nothing overridden, so a 1 MiB max unzipped page |
| write variant | `no-shuffle` |
| products read | `momentum` |
| access patterns | sequential, plus scatter at distance 1, 2, 4, 8, 16, 32, 64, 128, 256 |
| OS cache | cold, dropped before every repetition |
| cluster cache | off |
| implicit MT | off |
| repetitions | 2 |
| cases | 4 datasets x 10 patterns = 40 |

### Results: Where the time goes

Two RNTuple counters split the wall time: `timeWallRead` is time in storage I/O and
`timeWallUnzip` is time decompressing. The remainder is deserialisation, index lookup and
filling the output vectors. Because `cluster_cache` is off there is no background reader
thread, so both counters sit on the critical path and their shares add up.

One figure per access pattern, with the four datasets side by side so the same pattern can be
compared across granularities. Each bar is the measured wall time split into its three parts,
and the y scale is linear, so a segment's height is its share of the bar.

<table align="center">
<tr>
<td align="center"><b>Sequential access</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/sequential.png" alt="Sequential access" width="360"></td>
<td align="center"><b>Scatter access, distance 1</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-001.png" alt="Scatter access, distance 1" width="360"></td>
</tr>
<tr>
<td align="center"><b>Scatter access, distance 2</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-002.png" alt="Scatter access, distance 2" width="360"></td>
<td align="center"><b>Scatter access, distance 4</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-004.png" alt="Scatter access, distance 4" width="360"></td>
</tr>
<tr>
<td align="center"><b>Scatter access, distance 8</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-008.png" alt="Scatter access, distance 8" width="360"></td>
<td align="center"><b>Scatter access, distance 16</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-016.png" alt="Scatter access, distance 16" width="360"></td>
</tr>
<tr>
<td align="center"><b>Scatter access, distance 32</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-032.png" alt="Scatter access, distance 32" width="360"></td>
<td align="center"><b>Scatter access, distance 64</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-064.png" alt="Scatter access, distance 64" width="360"></td>
</tr>
<tr>
<td align="center"><b>Scatter access, distance 128</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-128.png" alt="Scatter access, distance 128" width="360"></td>
<td align="center"><b>Scatter access, distance 256</b><br><img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/bottleneck/scatter-256.png" alt="Scatter access, distance 256" width="360"></td>
</tr>
</table>

<details>
<summary>Full numbers behind the plots</summary>

**Wall time per pass, seconds**

| access | 1.00 evt/page | 3.96 evt/page | 253 evt/page | 4,045 evt/page |
|---|---:|---:|---:|---:|
| sequential | 3.18 | 3.40 | 3.37 | 4.03 |
| scatter-1 | 3.21 | 4.21 | 4.07 | 4.89 |
| scatter-2 | 3.18 | 4.61 | 4.73 | 5.50 |
| scatter-4 | 3.20 | 5.09 | 6.43 | 7.14 |
| scatter-8 | 3.20 | 5.52 | 9.24 | 9.82 |
| scatter-16 | 3.23 | 5.78 | 15.19 | 15.67 |
| scatter-32 | 3.21 | 5.91 | 26.84 | 27.84 |
| scatter-64 | 3.21 | 5.99 | 50.12 | 51.42 |
| scatter-128 | 3.20 | 6.11 | 93.80 | 98.40 |
| scatter-256 | 3.19 | 6.19 | 140.66 | 193.31 |

**Storage I/O as a share of wall time, per cent**

| access | 1.00 evt/page | 3.96 evt/page | 253 evt/page | 4,045 evt/page |
|---|---:|---:|---:|---:|
| sequential | 21.8 | 22.4 | 23.2 | 21.4 |
| scatter-1 | 21.7 | 19.6 | 20.9 | 18.4 |
| scatter-2 | 22.0 | 18.6 | 19.2 | 17.2 |
| scatter-4 | 22.2 | 18.0 | 17.0 | 15.3 |
| scatter-8 | 22.4 | 17.5 | 13.9 | 12.7 |
| scatter-16 | 22.6 | 17.9 | 13.5 | 12.3 |
| scatter-32 | 22.3 | 17.9 | 10.9 | 10.2 |
| scatter-64 | 22.6 | 17.2 | 9.0 | 8.6 |
| scatter-128 | 22.4 | 17.7 | 8.9 | 7.6 |
| scatter-256 | 22.7 | 17.9 | 8.8 | 6.9 |

**Decompression as a share of wall time, per cent**

| access | 1.00 evt/page | 3.96 evt/page | 253 evt/page | 4,045 evt/page |
|---|---:|---:|---:|---:|
| sequential | 27.9 | 28.0 | 24.8 | 23.7 |
| scatter-1 | 28.0 | 39.9 | 36.0 | 36.0 |
| scatter-2 | 28.1 | 44.1 | 44.3 | 42.5 |
| scatter-4 | 28.0 | 49.6 | 56.2 | 53.1 |
| scatter-8 | 28.0 | 52.4 | 67.3 | 65.0 |
| scatter-16 | 27.9 | 54.0 | 74.6 | 73.7 |
| scatter-32 | 28.0 | 54.6 | 82.2 | 81.8 |
| scatter-64 | 27.7 | 55.0 | 87.3 | 86.9 |
| scatter-128 | 27.9 | 54.9 | 88.9 | 89.9 |
| scatter-256 | 27.6 | 54.7 | 89.7 | 91.7 |

</details>

**Growth from sequential to scatter-256**

| events per page | bytes read | I/O time | decompression time | wall time |
|---|---:|---:|---:|---:|
| 1.00 | 1.0x | 1.0x | 1.0x | 1.0x |
| 3.96 | 3.9x | 1.5x | 3.6x | 1.8x |
| 253 | 157.1x | 15.8x | 151.2x | 41.8x |
| 4,045 | 215.0x | 15.4x | 186.1x | 48.0x |

### The answer

**The control case has no penalty at all, so scattered offsets by themselves cost nothing.**
At one event per page, no permutation can make the reader touch a page twice, and the pass
takes between 3.18 and 3.23 s across all ten patterns, a spread under 2 per cent, with the
bytes fetched from storage fixed at 1,016 MiB. Everything reported below therefore comes from
revisiting pages, not from the addresses being out of order.

**Storage I/O never holds the largest share.** In all forty cases the largest component is
either decompression or the deserialise-and-fill remainder. Even in the cheapest case, a
sequential pass, storage takes 21 to 23 per cent of the wall time.

**The I/O share falls exactly where the cost rises.** At 4,045 events per page, going from
sequential to scatter-256 makes the pass 48 times slower. Over that same span the storage
share drops from 21.4 to 6.9 per cent while decompression climbs from 23.7 to 91.7 per cent.
The component that grows into the cost is decompression, not storage.

**Storage absorbs the extra work, the decompressor does not.** At 4,045 events per page the
reader pulls 215 times more bytes at scatter-256 than at sequential but spends only 15 times
longer doing it, because a page that was already fetched comes back from the operating
system's cache instead of from the disk. Decompression has no such escape: its time follows
the byte count almost exactly, 215 times the bytes for 186 times the time. Every revisited
page is unsealed again in full.

So the cost of an out-of-order read is a decompression cost, and predicting it reduces to
predicting how many bytes get decompressed. That is what study two measures.

## 3.4 Study two: the multiplier is the number of events sharing a page

Study one showed the cost is decompression. This study asks what sets its size, and 3.2
predicts the answer: a page is unsealed once per visit, so the waste is bounded by how many
events share a page.

The axes file is the one walked through in 2.4. Everything is held where study one had it
except the two settings in bold, and the datasets, which go from four to seven.




| dataset | events | particles per event | raw payload per event | events per page | pages in the widest column |
|---|---:|---:|---:|---:|---:|
| `s400_p262144-262144` | 400 | 262,144 | 6 MiB | 1.00 | 400 |
| `s1600_p65536-65536` | 1,600 | 65,536 | 1.5 MiB | 3.96 | 404 |
| `s6400_p16384-16384` | 6,400 | 16,384 | 0.375 MiB | 15.8 | 405 |
| `s25600_p4096-4096` | 25,600 | 4,096 | 0.094 MiB | 63.2 | 405 |
| `s102400_p1024-1024` | 102,400 | 1,024 | 0.023 MiB | 253 | 405 |
| `s409600_p256-256` | 409,600 | 256 | 0.006 MiB | 1,014 | 404 |
| `s1638400_p64-64` | 1,638,400 | 64 | 0.001 MiB | 4,045 | 405 |

| Setting | Study two |
|---|---|
| write options | ROOT's defaults, nothing overridden, so a 1 MiB max unzipped page |
| write variant | `no-shuffle` |
| products read | `momentum` |
| access patterns | sequential, plus scatter at distance 1, 2, 4, 8, 16, 32, 64, 128, 256 |
| OS cache | cold, dropped before every repetition |
| cluster cache | **on** |
| implicit MT | off |
| repetitions | **1** |
| cases | 7 datasets x 10 patterns = 70 |

Two ratios are recorded per case, both against the sequential baseline of 1.0:

- **pages unzipped / pages in file**, how many times the average page was decompressed;
- **decompressed / wanted bytes**, how many bytes came out of the decompressor for every byte
  the reader asked for (the wanted bytes are the particles read times 12, the momentum
  triplet).

<p align="center"><b>Pages unzipped / pages in file</b></p>
<p align="center">
<img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/read_cost_vs_event_size__pg1mib__no-shuffle__page_unseal_amplification.png" alt="Pages unzipped over pages in file, against event size" width="640">
</p>

<p align="center"><b>Decompressed / wanted bytes</b></p>
<p align="center">
<img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/read_cost_vs_event_size__pg1mib__no-shuffle__decomp_amplification.png" alt="Decompressed over wanted bytes, against event size" width="640">
</p>

### Results: what the figures show

Both figures have the same shape. Each scatter distance holds a flat plateau across the
small-event datasets, at a height set by the distance, then falls away as events get bigger,
and all ten patterns converge where one event fills a page, on 1.0 exactly in the byte figure.
Sequential is flat at 1.0 throughout.

**The two figures are the same curve.** The byte ratio and the page ratio trace each other
plateau for plateau, so the decompressed volume is explained by page re-unsealing and by
nothing else. That is what turns "decompression grows" into a mechanism. They come apart only
at the largest events, where the extra unsealings are the tiny `event_id` and offsets pages
and carry no real bytes.

**On the plateau, the cost is set by the distance, not by the dataset.** Where a page holds far
more events than the reader drifts, the curves for different datasets lie on top of each other
and doubling the distance roughly doubles the amplification, from about 8x at distance 8 to
about 220x at 256. Drifting `d` events out of order costs on the order of `d` times the volume
actually needed, a little under it in practice.

**The plateau ends where a page stops holding many events.** A page cannot be unsealed more
times than it has events in it, so the amplification is capped by the events per page, and
that cap is what pulls every curve down as events grow. 

> **Note.** Read the ceiling off the byte ratio, not the page ratio. Pages are counted across
> all five columns of the container, so the handful of `event_id` and offsets pages are
> re-unsealed too and inflate the count without carrying any payload. At large events they are
> most of the count, which is why the page column sits above the ceiling while the byte column
> lands on it.

At one event per page every pattern costs the same as sequential, which is study one's control
seen from the other side: no visit order can revisit a page that holds a single event.

<details>
<summary>Every case, as a heatmap</summary>

<p align="center">
<img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/heatmap_scatter_vs_event_size__pg1mib__no-shuffle__decomp_amplification.png" alt="Decompressed over wanted bytes, every dataset against every scatter distance" width="760">
</p>
<p align="center">
<img src="https://raw.githubusercontent.com/Ahmed-Idani/gsoc-2026-report-assets/main/heatmap_scatter_vs_event_size__pg1mib__no-shuffle__page_unseal_amplification.png" alt="Pages unzipped over pages in file, every dataset against every scatter distance" width="760">
</p>

Rows are datasets, coarsest at the bottom, columns are scatter distances. Every number quoted
above appears in these two grids.

</details>


# 4. What's left

# 5. Challenges and lessons

# Acknowledgments
