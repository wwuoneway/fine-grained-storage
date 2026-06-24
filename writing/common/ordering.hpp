#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fgs {

  // Build the order in which events are WRITTEN to disk (their physical storage
  // layout) for a given variant. Returns a permutation of [0, n) that the writer
  // iterates over, writing each event in that sequence — so this vector is what
  // determines where each event physically lands in the file. It is not the
  // read-time access pattern, which is a separate downstream axis.
  //   "no-shuffle" -> identity (0,1,2,...): events stored in event-id order.
  //   "shuffle"    -> seeded permutation (e.g. 3,10,1,5,...), reproducible per
  //                   seed: events scattered across the file, so reading them in
  //                   event-id order means non-sequential I/O.
  // Shared by every write strategy so the shuffle logic lives in one place.
  std::vector<std::uint64_t> write_order(std::uint64_t n,
                                         std::string const& variant,
                                         std::uint64_t seed);

}
