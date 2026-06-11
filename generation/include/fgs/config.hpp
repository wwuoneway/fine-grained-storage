#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fgs {

  struct PositionConfig {
    double min = -100.0;
    double max = 100.0;
  };

  struct MomentumConfig {
    double mean = 0.0;
    double stddev = 1.0;
    double min_magnitude = 0.05;
  };
  struct ParticleConfig {
    std::uint32_t min = 2;
    std::uint32_t max = 10;
  };

  struct Config {
    std::uint64_t num_events = 1000;
    ParticleConfig particles{};
    std::uint64_t seed = 42;
    std::string output_dir = "output";
    PositionConfig position{};
    MomentumConfig momentum{};
  };

  Config load_config(std::filesystem::path const& path);

}
