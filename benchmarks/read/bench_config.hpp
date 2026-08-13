#pragma once

// Benchmark case description and configuration parsing for the read benchmark:
// the JSON schema (BenchmarkCase), how a case is read from config, and how its
// read_options map onto ROOT. Kept separate from the run/report logic so the
// schema is the single source of truth for what a benchmark case is.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <nlohmann/json_fwd.hpp>

namespace fgs::bench {

  enum class CacheState { Cold, Warm };

  struct BenchmarkCase {
    bool enabled = true;
    int benchmark_num = 0;
    std::string name;
    std::string description;
    std::string variant;
    std::filesystem::path root_file;
    std::filesystem::path manifest_file;
    std::vector<std::string> products; // empty = all
    std::uint64_t num_events = 0;
    std::string access_pattern = "sequential";
    std::uint64_t access_seed = 1234; // random-pattern permutation seed
    std::uint64_t stride = 16;         // strided-pattern jump distance
    std::uint64_t scatter_distance = 1; // scatter-pattern max swap distance
    std::uint64_t scatter_seed = 1234;  // scatter-pattern swap seed
    std::uint64_t repetitions = 1;
    CacheState cache_state = CacheState::Cold;
    std::string evict_method = "posix_fadvise";
    bool warmup = false;
    std::string cluster_cache = "off";
    std::string implicit_mt = "off";
  };

  std::string cache_state_name(CacheState state);

  // Load and parse a JSON file (top-level config or a strategy manifest); throws
  // if it cannot be opened.
  nlohmann::json load_json(std::filesystem::path const& path);

  BenchmarkCase parse_benchmark(nlohmann::json const& j);

  ROOT::RNTupleReadOptions make_read_options(BenchmarkCase const& bench);

}
