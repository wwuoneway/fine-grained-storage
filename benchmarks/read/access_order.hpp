#pragma once

// Event-id visitation order for the read benchmark. A small polymorphic
// generator so the access pattern (sequential / random / strided / scatter) is
// a single, testable responsibility, advanced one id at a time inside the read
// loop.
//
// Sequential and strided compute ids on the fly (no storage); random and
// scatter materialize a permutation. This keeps large-N runs (e.g. 2e7 events)
// allocation-free unless one of those two is explicitly requested.

#include <cstdint>
#include <memory>
#include <string>

namespace fgs::bench {

  // Realized displacement from the sequential position, to check a requested
  // scatter distance against what the clamp at the array ends allowed.
  struct OrderStats {
    double mean_displacement = 0.0;
    std::uint64_t max_displacement = 0;
  };

  // Yields each event id in [0, n) exactly once. The read loop calls next()
  // exactly n times; behaviour past n calls is unspecified.
  class EventOrder {
  public:
    virtual std::uint64_t next() = 0;
    virtual OrderStats stats() const { return {}; }
    virtual ~EventOrder() = default;
  };

  struct OrderSpec {
    std::string pattern = "sequential";
    std::uint64_t n = 0;
    std::uint64_t seed = 1234; // random pattern
    std::uint64_t stride = 16; // strided pattern
    std::uint64_t scatter_distance = 1;
    std::uint64_t scatter_seed = 1234; // scatter pattern
  };

  // Build the generator for a pattern. Throws on an unknown pattern, and for
  // the strided pattern on stride == 0 or stride >= n (which would degenerate
  // to a sequential order).
  //   sequential : 0,1,...,n-1          (best-case locality)
  //   random     : Fisher-Yates shuffle (mt19937_64 seeded by `seed`)
  //   strided    : hop by `stride`       (full permutation)
  //   scatter    : near-sequential, each position swapped with one at most
  //                `scatter_distance` away (mt19937_64 seeded by `scatter_seed`)
  std::unique_ptr<EventOrder> make_event_order(OrderSpec const& spec);

}
