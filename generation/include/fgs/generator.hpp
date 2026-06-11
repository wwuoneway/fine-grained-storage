#pragma once

#include <cstdint>
#include <filesystem>
#include <random>

#include "fgs/config.hpp"
#include "fgs/types.hpp"

namespace fgs {

  Event generate_event(Config const& cfg, std::uint64_t event_id, std::mt19937_64& rng);

  // Summary statistics returned by a generation run, used for the stdout report.
  struct RunStats {
    std::uint64_t num_events = 0;
    std::uint64_t total_particles = 0;
    std::uint64_t positions_bytes = 0;
    std::uint64_t momenta_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint32_t min_particles = 0;
    std::uint32_t max_particles = 0;
    std::filesystem::path output_dir;    // resolved <output_dir>
    std::filesystem::path manifest_path; // <output_dir>/manifest.json
  };

  RunStats run_generation(Config const& cfg);

}
