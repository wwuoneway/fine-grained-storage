#include "access_order.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace fgs::bench {

  namespace {

    // 0,1,2,... — just a counter, no storage.
    class SequentialOrder : public EventOrder {
    public:
      std::uint64_t next() override { return cur_++; }

    private:
      std::uint64_t cur_ = 0;
    };

    // Full permutation that hops by `stride`, computed on the fly: walk the phase
    // 0 progression (0, stride, 2*stride, ...) until it passes n, then restart at
    // the next phase offset. Over exactly n calls every id 0..n-1 appears once.
    class StridedOrder : public EventOrder {
    public:
      StridedOrder(std::uint64_t n, std::uint64_t stride) : n_(n), stride_(stride) {}

      std::uint64_t next() override
      {
        std::uint64_t const id = cur_;
        cur_ += stride_;
        if (cur_ >= n_)
          cur_ = ++off_; // advance to the next phase offset
        return id;
      }

    private:
      std::uint64_t n_;
      std::uint64_t stride_;
      std::uint64_t off_ = 0;
      std::uint64_t cur_ = 0;
    };

    // The only pattern that allocates: one shuffled permutation held for the run.
    class RandomOrder : public EventOrder {
    public:
      RandomOrder(std::uint64_t n, std::uint64_t seed) : order_(n)
      {
        std::iota(order_.begin(), order_.end(), std::uint64_t{0});
        std::mt19937_64 rng(seed);
        std::shuffle(order_.begin(), order_.end(), rng); // Fisher-Yates
      }

      std::uint64_t next() override { return order_[i_++]; }

    private:
      std::vector<std::uint64_t> order_;
      std::size_t i_ = 0;
    };

  }

  std::unique_ptr<EventOrder> make_event_order(std::string const& pattern,
                                               std::uint64_t n,
                                               std::uint64_t seed,
                                               std::uint64_t stride)
  {
    if (pattern == "sequential")
      return std::make_unique<SequentialOrder>();
    if (pattern == "random")
      return std::make_unique<RandomOrder>(n, seed);
    if (pattern == "strided") {
      if (stride == 0)
        throw std::runtime_error("strided access_pattern requires stride > 0");
      return std::make_unique<StridedOrder>(n, stride);
    }
    throw std::runtime_error("unknown access_pattern \"" + pattern + "\"");
  }

}
