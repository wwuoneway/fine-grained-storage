#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <nlohmann/json.hpp>

#include "event_reader.hpp"

namespace fs = std::filesystem;

namespace {

  enum class CacheState { Cold, Warm };

  struct BenchmarkCase {
    bool enabled = true;
    std::string name;
    std::string description;
    std::string variant;
    fs::path root_file;
    fs::path manifest_file;
    std::uint64_t num_events = 0;
    std::string access_pattern = "sequential";
    std::uint64_t repetitions = 1;
    CacheState cache_state = CacheState::Cold;
    std::string evict_method = "posix_fadvise";
    bool warmup = false;
    std::string cluster_cache = "off";
    std::string implicit_mt = "off";
    bool metrics = false;
  };

  struct Measurement {
    std::uint64_t repetition = 0;
    double wall_s = 0.0;
    double latency_us_per_event = 0.0;
    std::uint64_t total_values = 0;
    double checksum = 0.0;
  };

  std::string cache_state_name(CacheState state)
  {
    return state == CacheState::Cold ? "cold" : "warm";
  }

  CacheState parse_cache_state(std::string const& value)
  {
    if (value == "cold")
      return CacheState::Cold;
    if (value == "warm")
      return CacheState::Warm;
    throw std::runtime_error("unknown os_cache.state \"" + value + "\"");
  }

  void evict_from_cache(fs::path const& path)
  {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("cannot open for cache eviction: " + path.string());

    int const rc = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);

    if (rc != 0)
      throw std::runtime_error("posix_fadvise(POSIX_FADV_DONTNEED) failed for " + path.string());
  }

  std::vector<fgs::ProductSpec> products_from_manifest(nlohmann::json const& manifest,
                                                       std::string& index_name)
  {
    std::vector<fgs::ProductSpec> products;
    for (auto const& p : manifest.at("products"))
      products.push_back(
        {p.at("name").get<std::string>(), p.at("product_id").get<std::uint64_t>()});
    index_name = manifest.at("products").at(0).at("index_container").get<std::string>();
    return products;
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

    opts.SetEnableMetrics(bench.metrics);
    return opts;
  }

  std::vector<std::uint64_t> make_event_ids(std::uint64_t n_events,
                                            std::string const& access_pattern)
  {
    if (access_pattern != "sequential")
      throw std::runtime_error("unsupported access_pattern \"" + access_pattern + "\"");

    std::vector<std::uint64_t> ids(n_events);
    std::iota(ids.begin(), ids.end(), std::uint64_t{0});
    return ids;
  }

  void print_benchmark_header(BenchmarkCase const& bench,
                              fs::path const& root_path,
                              std::uint64_t n_events_used)
  {
    std::cout << "\n=== " << bench.name << " ===\n"
              << "variant         : " << bench.variant << "\n"
              << "root file       : " << root_path << "\n"
              << "access_pattern  : " << bench.access_pattern << "\n"
              << "events config   : " << bench.num_events << "\n"
              << "events used     : " << n_events_used << "\n"
              << "repetitions     : " << bench.repetitions << "\n"
              << "os cache        : " << cache_state_name(bench.cache_state) << "\n"
              << "cluster cache   : " << bench.cluster_cache << "\n";
  }

  BenchmarkCase parse_benchmark(nlohmann::json const& j)
  {
    BenchmarkCase bench;
    bench.enabled = j.value("enabled", true);

    auto const& metadata = j.at("metadata");
    bench.name = metadata.at("name").get<std::string>();
    bench.description = metadata.value("description", "");
    bench.variant = metadata.at("variant").get<std::string>();

    auto const& input = j.at("input_data");
    bench.root_file = input.at("root_file").get<std::string>();
    bench.manifest_file = input.at("manifest_file").get<std::string>();

    bench.num_events = j.at("num_events").get<std::uint64_t>();
    bench.access_pattern = j.value("access_pattern", "sequential");
    bench.repetitions = j.value("repetitions", std::uint64_t{1});

    auto const& os_cache = j.at("os_cache");
    bench.cache_state = parse_cache_state(os_cache.at("state").get<std::string>());
    bench.evict_method = os_cache.value("evict_method", "posix_fadvise");
    bench.warmup = os_cache.value("warmup", bench.cache_state == CacheState::Warm);

    auto const& read_options = j.at("read_options");
    bench.cluster_cache = read_options.value("cluster_cache", "off");
    bench.implicit_mt = read_options.value("implicit_mt", "off");
    bench.metrics = read_options.value("metrics", false);

    if (bench.num_events == 0)
      throw std::runtime_error("benchmark \"" + bench.name + "\" has num_events=0");
    if (bench.repetitions == 0)
      throw std::runtime_error("benchmark \"" + bench.name + "\" has repetitions=0");
    if (bench.cache_state == CacheState::Cold && bench.evict_method != "posix_fadvise")
      throw std::runtime_error("benchmark \"" + bench.name +
                               "\" only supports cold evict_method=posix_fadvise for now");
    if (bench.cache_state == CacheState::Warm && bench.evict_method != "none")
      throw std::runtime_error("benchmark \"" + bench.name +
                               "\" only supports warm evict_method=none for now");

    return bench;
  }

  [[maybe_unused]] double consume_values(std::vector<float> const& values)
  {
    return std::accumulate(values.begin(), values.end(), 0.0);
  }

  Measurement read_once(BenchmarkCase const& bench,
                        fs::path const& root_path,
                        std::string const& index_name,
                        std::vector<fgs::ProductSpec> const& products,
                        std::vector<std::uint64_t> const& event_ids,
                        std::uint64_t repetition)
  {
    ROOT::RNTupleReadOptions opts = make_read_options(bench);
    fgs::EventReader reader(root_path, index_name, products, opts);

    Measurement m;
    m.repetition = repetition;

    //sequential read
    auto const start = std::chrono::steady_clock::now();
    for (std::uint64_t event_id : event_ids) {
      for (fgs::ProductSpec const& product : products) {
        std::vector<float> values = reader.read_product(event_id, product.name);
        (void)values;
      }
    }
    auto const stop = std::chrono::steady_clock::now();

    //
    // Keep the timer focused on the read loop
    m.wall_s = std::chrono::duration<double>(stop - start).count();
    m.latency_us_per_event = m.wall_s / static_cast<double>(event_ids.size()) * 1.0e6;

    if (bench.metrics)
      reader.print_metrics();

    return m;
  }

  void warm_cache(BenchmarkCase const& bench,
                  fs::path const& root_path,
                  std::string const& index_name,
                  std::vector<fgs::ProductSpec> const& products,
                  std::vector<std::uint64_t> const& event_ids)
  {
    BenchmarkCase warmup_case = bench;
    warmup_case.metrics = false;
    (void)read_once(warmup_case, root_path, index_name, products, event_ids, 0);
  }

  void run_read_benchmark(BenchmarkCase const& bench)
  {
    nlohmann::json manifest = load_json(bench.manifest_file);
    std::string index_name;
    std::vector<fgs::ProductSpec> products = products_from_manifest(manifest, index_name);
    fs::path root_path = bench.root_file;
    std::vector<fs::path> container_paths{root_path};

    fgs::EventReader probe(root_path, index_name, products, make_read_options(bench));
    std::uint64_t const n_events = std::min(bench.num_events, probe.num_events());
    std::vector<std::uint64_t> event_ids = make_event_ids(n_events, bench.access_pattern);

    print_benchmark_header(bench, root_path, n_events);

    for (std::uint64_t rep = 1; rep <= bench.repetitions; ++rep) {
      if (bench.cache_state == CacheState::Cold) {
        for (fs::path const& path : container_paths)
          evict_from_cache(path);
      } else if (bench.warmup) {
        warm_cache(bench, root_path, index_name, products, event_ids);
      }

      Measurement m = read_once(bench, root_path, index_name, products, event_ids, rep);

      std::cout << "rep " << rep << " wall_s=" << m.wall_s
                << " latency_us_per_event=" << m.latency_us_per_event
                << " values=" << m.total_values << "\n";
    }
  }

}

int main(int argc, char** argv)
{
  fs::path config_path =
    argc > 1 ? fs::path{argv[1]} : fs::path{"configs/benchmarks/reading_benchmarks.json"};

  try {
    nlohmann::json config = load_json(config_path);
    for (auto const& item : config.at("benchmarks")) {
      BenchmarkCase bench = parse_benchmark(item);
      if (!bench.enabled)
        continue;
      run_read_benchmark(bench);
    }
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fgs_read_bench: error: " << e.what() << "\n";
    return 1;
  }
}
