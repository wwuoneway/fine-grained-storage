#include "common/ordering.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>

namespace fgs {

  std::vector<std::uint64_t> write_order(std::uint64_t n,
                                         std::string const& variant,
                                         std::uint64_t seed)
  {
    std::vector<std::uint64_t> order(n);
    std::iota(order.begin(), order.end(), std::uint64_t{0});

    if (variant == "no-shuffle")
      return order;
    if (variant == "shuffle") {
      std::shuffle(order.begin(), order.end(), std::mt19937_64{seed});
      return order;
    }
    throw std::runtime_error("write_order: unknown variant \"" + variant +
                             "\" (expected \"no-shuffle\" or \"shuffle\")");
  }

}
