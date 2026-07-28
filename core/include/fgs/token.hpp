#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace fgs {

  // Locates one product of one event within the file: the container (RNTuple)
  // holding it and the entry (row) within that container.
  struct Token {
    std::string container;
    std::uint64_t entry = 0;
  };

  // Value column of the two-column event index (event_id, index_value): the
  // per-product tokens for one event.
  using EventIndex = std::map<std::string, Token>;

}
