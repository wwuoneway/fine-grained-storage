#include "bench_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <nlohmann/json.hpp>

#include "access_order.hpp"
#include "bench_config.hpp"
#include "bench_report.hpp"
#include "event_reader.hpp"
#include "os_cache.hpp"

namespace fs = std::filesystem;

namespace fgs::bench {

  namespace {

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

      // Build the visitation order before the timer so ordering cost never enters
      // the measured region. Sequential/strided are allocation-free; only random
      // materializes a permutation (see access_order.hpp).
      std::unique_ptr<EventOrder> order =
        make_event_order(bench.access_pattern, n_events, bench.access_seed, bench.stride);

      // Cheap dead-code-elimination sink: counting the values read keeps `values`
      // used so the optimizer can't drop the read loop, without adding arithmetic
      // weight (a full-value checksum) to the timed region.
      std::uint64_t total_values = 0;

      // Visit n_events events in the pattern's order (order->next() yields each id
      // in [0, n_events) exactly once).
      auto const start = std::chrono::steady_clock::now();
      for (std::uint64_t i = 0; i < n_events; ++i) {
        std::uint64_t const event_id = order->next();
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

    // One self-contained folder per benchmark, under the run's benchmarks/ dir.
    fs::path const bench_dir =
      run_dir / "benchmarks" / ("benchmark_" + std::to_string(bench.benchmark_num));
    fs::create_directories(bench_dir / "csv");
    fs::create_directories(bench_dir / "runs");

    fgs::bench::BenchmarkId id{bench.benchmark_num,
                               bench.name,
                               bench.description,
                               bench.variant,
                               bench.access_pattern,
                               cache_state_name(bench.cache_state),
                               bench.cluster_cache,
                               bench.implicit_mt,
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
      Measurement m = read_once(bench, root_path, index_name, products, n_events, rep, metrics_raw);
      if (bench.metrics)
        m.counters = fgs::bench::parse_read_counters(metrics_raw);

      std::ostringstream line;
      line << "rep " << rep << " wall_s=" << m.wall_s
           << " latency_us_per_event=" << m.latency_us_per_event
           << " throughput_evt_s=" << m.throughput_evt_s << " values=" << m.total_values
           << " read_ms=" << m.counters.read_wall_ms << " unzip_ms=" << m.counters.unzip_wall_ms
           << " n_read=" << m.counters.n_read << "\n\n";
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
