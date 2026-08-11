#include "access_order.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace fgs::bench {

  namespace {

    // 0,1,2,... -- just a counter, no storage.
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
        if (cur_ >= n_) {
          ++off_;
          if (off_ >= n_)
            off_ = 0; // return to the start
          cur_ = off_;
        }
        return id;
      }

    private:
      std::uint64_t n_;
      std::uint64_t stride_;
      std::uint64_t off_ = 0;
      std::uint64_t cur_ = 0;
    };

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

    // The seed is required, not a convenience: read_once builds the order once
    // for the timed pass and again for the instrumented pass, and the two must
    // visit the same ids in the same order for their counters to be comparable.
    class ScatterOrder : public EventOrder {
    public:
      ScatterOrder(std::uint64_t n, std::uint64_t distance, std::uint64_t seed) : order_(n)
      {
        std::iota(order_.begin(), order_.end(), std::uint64_t{0});
        if (distance == 0 || n < 2)
          return; // identity, a deliberate sequential baseline rather than an error
        auto const d = static_cast<std::int64_t>(distance);
        auto const last = static_cast<std::int64_t>(n) - 1;
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<std::int64_t> jump(-d, d);
        for (std::int64_t i = 0; i <= last; ++i)
          std::swap(order_[i], order_[std::clamp(i + jump(rng), std::int64_t{0}, last)]);
      }

      std::uint64_t next() override { return order_[i_++]; }

      OrderStats stats() const override
      {
        OrderStats s;
        double sum = 0.0;
        for (std::size_t i = 0; i < order_.size(); ++i) {
          auto const disp = static_cast<std::uint64_t>(
            std::llabs(static_cast<std::int64_t>(order_[i]) - static_cast<std::int64_t>(i)));
          sum += static_cast<double>(disp);
          s.max_displacement = std::max(s.max_displacement, disp);
        }
        if (!order_.empty())
          s.mean_displacement = sum / static_cast<double>(order_.size());
        return s;
      }

    private:
      std::vector<std::uint64_t> order_;
      std::size_t i_ = 0;
    };

  }

  std::unique_ptr<EventOrder> make_event_order(OrderSpec const& spec)
  {
    std::string const& pattern = spec.pattern;
    std::uint64_t const n = spec.n;

    if (pattern == "sequential")
      return std::make_unique<SequentialOrder>();
    if (pattern == "random") {
      if (n < 2)
        std::cerr << "warning: access_pattern=random with num_events=" << n
                  << " is trivially sequential\n";
      return std::make_unique<RandomOrder>(n, spec.seed);
    }
    if (pattern == "scatter")
      return std::make_unique<ScatterOrder>(n, spec.scatter_distance, spec.scatter_seed);
    if (pattern == "strided") {
      std::uint64_t const stride = spec.stride;
      if (stride == 0)
        throw std::runtime_error("strided access_pattern requires stride > 0");
      // stride >= n makes every hop wrap immediately: a sequential order
      // mislabelled as strided.
      if (stride >= n)
        throw std::runtime_error("strided access_pattern requires stride < num_events (stride=" +
                                 std::to_string(stride) + ", num_events=" + std::to_string(n) +
                                 ")");
      return std::make_unique<StridedOrder>(n, stride);
    }
    throw std::runtime_error("unknown access_pattern \"" + pattern + "\"");
  }

}
