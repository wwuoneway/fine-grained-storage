#include "bench_config.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace fgs::bench {

  namespace {

    CacheState parse_cache_state(std::string const& value)
    {
      if (value == "cold")
        return CacheState::Cold;
      if (value == "warm")
        return CacheState::Warm;
      throw std::runtime_error("unknown os_cache.state \"" + value + "\"");
    }

  }

  std::string cache_state_name(CacheState state)
  {
    return state == CacheState::Cold ? "cold" : "warm";
  }

  nlohmann::json load_json(fs::path const& path)
  {
    std::ifstream in(path);
    if (!in)
      throw std::runtime_error("cannot open " + path.string());
    nlohmann::json j;
    in >> j;
    return j;
  }

  ROOT::RNTupleReadOptions make_read_options(BenchmarkCase const& bench)
  {
    ROOT::RNTupleReadOptions opts;

    if (bench.cluster_cache == "off") {
      opts.SetClusterCache(ROOT::RNTupleReadOptions::EClusterCache::kOff);
    } else if (bench.cluster_cache == "on") {
      opts.SetClusterCache(ROOT::RNTupleReadOptions::EClusterCache::kOn);
    } else if (bench.cluster_cache != "default") {
      throw std::runtime_error("unknown read_options.cluster_cache \"" + bench.cluster_cache +
                               "\"");
    }

    if (bench.implicit_mt == "off") {
      opts.SetUseImplicitMT(ROOT::RNTupleReadOptions::EImplicitMT::kOff);
    } else if (bench.implicit_mt != "default") {
      throw std::runtime_error("unknown read_options.implicit_mt \"" + bench.implicit_mt + "\"");
    }

    opts.SetEnableMetrics(true);
    return opts;
  }

  BenchmarkCase parse_benchmark(nlohmann::json const& j)
  {
    BenchmarkCase bench;
    bench.enabled = j.value("enabled", true);

    auto const& metadata = j.at("metadata");
    bench.benchmark_num = metadata.value("benchmark_num", 0);
    bench.name = metadata.at("name").get<std::string>();
    bench.description = metadata.value("description", "");
    bench.variant = metadata.at("variant").get<std::string>();

    auto const& input = j.at("input_data");
    bench.root_file = input.at("root_file").get<std::string>();
    bench.manifest_file = input.at("manifest_file").get<std::string>();

    bench.num_events = j.at("num_events").get<std::uint64_t>();
    bench.access_pattern = j.value("access_pattern", "sequential");
    bench.access_seed = j.value("access_seed", std::uint64_t{1234});
    bench.stride = j.value("stride", std::uint64_t{16});
    bench.repetitions = j.value("repetitions", std::uint64_t{1});

    auto const& os_cache = j.at("os_cache");
    bench.cache_state = parse_cache_state(os_cache.at("state").get<std::string>());
    bench.evict_method = os_cache.value("evict_method", "posix_fadvise");
    bench.warmup = os_cache.value("warmup", bench.cache_state == CacheState::Warm);

    auto const& read_options = j.at("read_options");
    bench.cluster_cache = read_options.value("cluster_cache", "off");
    bench.implicit_mt = read_options.value("implicit_mt", "off");

    if (bench.num_events == 0)
      throw std::runtime_error("benchmark \"" + bench.name + "\" has num_events=0");
    if (bench.repetitions == 0)
      throw std::runtime_error("benchmark \"" + bench.name + "\" has repetitions=0");
    if (bench.access_pattern != "sequential" && bench.access_pattern != "random" &&
        bench.access_pattern != "strided")
      throw std::runtime_error("benchmark \"" + bench.name + "\" has unknown access_pattern \"" +
                               bench.access_pattern + "\"");
    if (bench.stride == 0)
      throw std::runtime_error("benchmark \"" + bench.name + "\" has stride=0");
    if (bench.cache_state == CacheState::Cold && bench.evict_method != "posix_fadvise")
      throw std::runtime_error("benchmark \"" + bench.name +
                               "\" only supports cold evict_method=posix_fadvise for now");
    if (bench.cache_state == CacheState::Warm && bench.evict_method != "none")
      throw std::runtime_error("benchmark \"" + bench.name +
                               "\" only supports warm evict_method=none for now");

    return bench;
  }

}
