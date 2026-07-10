#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <nlohmann/json.hpp>

#include "bench_report.hpp"
#include "event_reader.hpp"

namespace fs = std::filesystem;

namespace {

  using fgs::bench::Measurement;

  enum class CacheState { Cold, Warm };

  struct BenchmarkCase {
    bool enabled = true;
    int benchmark_num = 0;
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

  // Write the same text to stdout and to the run.txt log, so there is always a
  // scannable plain-text record next to the CSVs.
  void tee(std::ostream& log, std::string const& text)
  {
    std::cout << text;
    log << text;
  }

  void print_benchmark_header(std::ostream& log,
                              BenchmarkCase const& bench,
                              fs::path const& root_path,
                              std::uint64_t n_events_used)
  {
    std::ostringstream os;
    os << "\n=== " << bench.name << " ===\n"
       << "variant         : " << bench.variant << "\n"
       << "root file       : " << root_path << "\n"
       << "access_pattern  : " << bench.access_pattern << "\n"
       << "events config   : " << bench.num_events << "\n"
       << "events used     : " << n_events_used << "\n"
       << "repetitions     : " << bench.repetitions << "\n"
       << "os cache        : " << cache_state_name(bench.cache_state) << "\n"
       << "cluster cache   : " << bench.cluster_cache << "\n";
    tee(log, os.str());
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

  Measurement read_once(BenchmarkCase const& bench,
                        fs::path const& root_path,
                        std::string const& index_name,
                        std::vector<fgs::ProductSpec> const& products,
                        std::uint64_t n_events,
                        std::uint64_t repetition,
                        std::string& metrics_raw)
  {
    ROOT::RNTupleReadOptions opts = make_read_options(bench);
    fgs::EventReader reader(root_path, index_name, products, opts);

    Measurement m;
    m.repetition = repetition;

    // Cheap dead-code-elimination sink: counting the values read keeps `values`
    // used so the optimizer can't drop the read loop, without adding arithmetic
    // weight (a full-value checksum) to the timed region.
    std::uint64_t total_values = 0;

    // Sequential read: visit event ids 0..n_events-1 in order. Random/strided
    // access (deliverable 3) would instead precompute a shuffled/strided vector
    // of event ids and iterate that here.
    auto const start = std::chrono::steady_clock::now();
    for (std::uint64_t event_id = 0; event_id < n_events; ++event_id) {
      for (fgs::ProductSpec const& product : products) {
        std::vector<float> values = reader.read_product(event_id, product.name);
        total_values += values.size();
      }
    }
    auto const stop = std::chrono::steady_clock::now();

    // Keep the timer focused on the read loop
    m.wall_s = std::chrono::duration<double>(stop - start).count();
    m.latency_us_per_event = m.wall_s / static_cast<double>(n_events) * 1.0e6;
    m.throughput_evt_s = m.wall_s > 0.0 ? static_cast<double>(n_events) / m.wall_s : 0.0;
    m.total_values = total_values;

    if (bench.metrics) {
      std::ostringstream raw;
      reader.print_metrics(raw);
      metrics_raw = raw.str();
    }

    return m;
  }

  void warm_cache(BenchmarkCase const& bench,
                  fs::path const& root_path,
                  std::string const& index_name,
                  std::vector<fgs::ProductSpec> const& products,
                  std::uint64_t n_events)
  {
    BenchmarkCase warmup_case = bench;
    warmup_case.metrics = false;
    std::string discard;
    (void)read_once(warmup_case, root_path, index_name, products, n_events, 0, discard);
  }

  void run_read_benchmark(BenchmarkCase const& bench,
                          fs::path const& run_dir,
                          std::ostream& summary_csv)
  {
    nlohmann::json manifest = load_json(bench.manifest_file);
    std::string index_name;
    std::vector<fgs::ProductSpec> products = products_from_manifest(manifest, index_name);
    fs::path root_path = bench.root_file;
    std::vector<fs::path> container_paths{root_path};

    std::uint64_t const n_events = [&] {
      ROOT::RNTupleReadOptions probe_opts;
      probe_opts.SetClusterCache(ROOT::RNTupleReadOptions::EClusterCache::kOff);
      fgs::EventReader probe(root_path, index_name, products, probe_opts);
      return std::min(bench.num_events, probe.num_events());
    }();
    // TODO: extend for random, strided access (deliverable 3). Until then the
    // read loop in read_once visits events sequentially, so reject anything else.
    if (bench.access_pattern != "sequential")
      throw std::runtime_error("unsupported access_pattern \"" + bench.access_pattern + "\"");

    // One self-contained folder per benchmark.
    fs::path const bench_dir = run_dir / ("benchmark_" + std::to_string(bench.benchmark_num));
    fs::create_directories(bench_dir / "csv");
    fs::create_directories(bench_dir / "runs");

    fgs::bench::BenchmarkId id{bench.benchmark_num,
                               bench.name,
                               bench.description,
                               bench.variant,
                               bench.access_pattern,
                               cache_state_name(bench.cache_state),
                               bench.cluster_cache,
                               n_events,
                               bench.repetitions,
                               bench.root_file.string(),
                               bench.manifest_file.string()};
    fgs::bench::write_benchmark_metadata(bench_dir / "metadata.txt", id);

    std::ofstream log(bench_dir / "benchmark.log");
    print_benchmark_header(log, bench, root_path, n_events);

    std::vector<Measurement> reps;
    reps.reserve(bench.repetitions);
    for (std::uint64_t rep = 1; rep <= bench.repetitions; ++rep) {
      if (bench.cache_state == CacheState::Cold) {
        for (fs::path const& path : container_paths)
          evict_from_cache(path);
      } else if (bench.warmup) {
        warm_cache(bench, root_path, index_name, products, n_events);
      }

      std::string const started = fgs::bench::timestamp_human();
      std::string metrics_raw;
      Measurement m =
        read_once(bench, root_path, index_name, products, n_events, rep, metrics_raw);

      std::ostringstream line;
      line << "rep " << rep << " wall_s=" << m.wall_s
           << " latency_us_per_event=" << m.latency_us_per_event
           << " throughput_evt_s=" << m.throughput_evt_s << " values=" << m.total_values << "\n\n";
      tee(log, line.str());

      fgs::bench::write_run_report(
        bench_dir / "runs" / ("run_" + std::to_string(rep) + ".txt"), id, m, started, metrics_raw);
      reps.push_back(m);
    }

    fgs::bench::write_raw_csv(bench_dir / "csv" / "raw.csv", id, reps);
    fgs::bench::write_benchmark_summary_txt(bench_dir / "summary.txt", id, reps);
    fgs::bench::append_summary_row(summary_csv, id, reps);
  }

}

int main(int argc, char** argv)
{
  fs::path config_path =
    argc > 1 ? fs::path{argv[1]} : fs::path{"configs/benchmarks/reading_benchmarks.json"};

  try {
    nlohmann::json config = load_json(config_path);

    // Each invocation gets its own timestamped run folder under a fixed base, so
    // re-running any config never overwrites earlier results. The run root holds
    // the shared run_info.json (specs + config) and summary.csv; each benchmark
    // gets its own self-contained subfolder.
    fs::path const base = "output/benchmarks/reading-benchmarks";
    fs::path const run_dir = base / fgs::bench::timestamp_now();
    fs::create_directories(run_dir);

    std::ofstream summary_csv(run_dir / "summary.csv");
    summary_csv << fgs::bench::summary_header();
    fgs::bench::write_run_info(run_dir / "run_info.json", config_path, config);

    int index = 0;
    int failures = 0;
    for (auto const& item : config.at("benchmarks")) {
      ++index;
      BenchmarkCase bench = parse_benchmark(item);
      if (bench.benchmark_num == 0)
        bench.benchmark_num = index; // default to config order when unset
      if (!bench.enabled)
        continue;
      try {
        run_read_benchmark(bench, run_dir, summary_csv);
      } catch (std::exception const& e) {
        ++failures;
        std::cerr << "fgs_read_bench: benchmark " << bench.benchmark_num << " (" << bench.name
                  << ") failed: " << e.what() << "\n";
      }
    }

    std::cout << "\nresults written to " << run_dir << "\n";
    if (failures > 0) {
      std::cerr << "fgs_read_bench: " << failures << " benchmark(s) failed\n";
      return 1;
    }
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fgs_read_bench: error: " << e.what() << "\n";
    return 1;
  }
}
