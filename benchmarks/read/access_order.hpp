#pragma once

// Event-id visitation order for the read benchmark. A small polymorphic
// generator so the access pattern (sequential / random / strided) is a single,
// testable responsibility, advanced one id at a time inside the read loop.
//
// Sequential and strided compute ids on the fly (no storage); only random
// materializes a permutation. This keeps large-N runs (e.g. 2e7 events)
// allocation-free unless a random pattern is explicitly requested.

#include <cstdint>
#include <memory>
#include <string>

namespace fgs::bench {

  // Yields each event id in [0, n) exactly once. The read loop calls next()
  // exactly n times; behaviour past n calls is unspecified.
  class EventOrder {
  public:
    virtual std::uint64_t next() = 0;
    virtual ~EventOrder() = default;
  };

  // Build the generator for a pattern. Throws on an unknown pattern, and for
  // the strided pattern on stride == 0 or stride >= n (which would degenerate
  // to a sequential order).
  //   sequential : 0,1,...,n-1          (best-case locality)
  //   random     : Fisher-Yates shuffle (mt19937_64 seeded by `seed`)
  //   strided    : hop by `stride`       (full permutation)
  std::unique_ptr<EventOrder> make_event_order(std::string const& pattern,
                                               std::uint64_t n,
                                               std::uint64_t seed,
                                               std::uint64_t stride);

}
