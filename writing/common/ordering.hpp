#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fgs {

  // Build the order in which events are written to disk for a given variant.
  // Returns a permutation of [0, n):
  //   "no-shuffle" -> identity (0,1,2,...)
  //   "shuffle"    -> seeded shuffle (e.g. 3,10,1,5,...), reproducible for a seed
  // Shared by every write strategy so shuffle logic is defined in one place.
  std::vector<std::uint64_t> event_order(std::uint64_t n,
                                         std::string const& variant,
                                         std::uint64_t seed);

}
