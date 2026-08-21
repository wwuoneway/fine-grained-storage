#include "bench_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <nlohmann/json.hpp>

#include "access_order.hpp"
#include "bench_config.hpp"
#include "bench_report.hpp"
#include "fgs/event_reader.hpp"
#include "os_cache.hpp"

namespace fs = std::filesystem;

namespace fgs::bench {

  namespace {

    std::string index_name_from_manifest(nlohmann::json const& manifest)
    {
      return manifest.at("products").at(0).at("index_container").get<std::string>();
    }

    OrderSpec order_spec(BenchmarkCase const& bench, std::uint64_t n_events)
    {
      return {bench.access_pattern,
              n_events,
              bench.access_seed,
              bench.scatter_distance,
              bench.scatter_seed};
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
                                std::uint64_t n_events_read,
                                std::string const& containers)
    {
      std::ostringstream os;
      os << "\n=== " << bench.name << " ===\n"
         << "variant         : " << bench.variant << "\n"
         << "root file       : " << root_path << "\n"
         << "containers      : " << containers << "\n"
         << "access_pattern  : " << bench.access_pattern << "\n"
         << "scatter_distance: " << bench.scatter_distance << "\n"
         << "events read     : " << n_events_read << "\n"
         << "repetitions     : " << bench.repetitions << "\n"
         << "os cache        : " << cache_state_name(bench.cache_state) << "\n"
         << "cluster cache   : " << bench.cluster_cache << "\n";
      tee(log, os.str());
    }

    Measurement read_once(BenchmarkCase const& bench,
                          fs::path const& root_path,
                          std::string const& index_name,
                          std::uint64_t n_events,
                          std::uint64_t repetition,
                          std::string& metrics_raw)
    {
      ROOT::RNTupleReadOptions opts = make_read_options(bench);
      fgs::EventReader reader(root_path, index_name, opts, bench.products);

      Measurement m;
      m.repetition = repetition;

      // Build the visitation order before the timer so ordering cost never enters
      // the measured region (see access_order.hpp).
      std::unique_ptr<EventOrder> order = make_event_order(order_spec(bench, n_events));

      std::vector<std::string> const& products = reader.product_names();
      std::vector<std::size_t> const& element_bytes = reader.product_element_bytes();
      // Per product, so each can be weighed by its own element width later.
      std::vector<std::uint64_t> elements(products.size(), 0);

      auto const start = std::chrono::steady_clock::now();
      for (std::uint64_t i = 0; i < n_events; ++i) {
        std::uint64_t const event_id = order->next();
        for (std::size_t p = 0; p < products.size(); ++p)
          elements[p] += reader.read_product(event_id, products[p]);
      }
      auto const stop = std::chrono::steady_clock::now();

      // Reported so a short read shows up as a number rather than a plausible time.
      std::uint64_t total_elements = 0;
      std::uint64_t wanted_bytes = 0;
      for (std::size_t p = 0; p < products.size(); ++p) {
        total_elements += elements[p];
        wanted_bytes += elements[p] * element_bytes[p];
      }

      m.wall_s = std::chrono::duration<double>(stop - start).count();
      m.latency_us_per_event = m.wall_s / static_cast<double>(n_events) * 1.0e6;
      m.throughput_evt_s = m.wall_s > 0.0 ? static_cast<double>(n_events) / m.wall_s : 0.0;
      m.total_elements = total_elements;
      m.wanted_bytes = wanted_bytes;

      std::ostringstream raw;
      reader.print_metrics(raw);
      metrics_raw = raw.str();

      return m;
    }

    void warm_cache(BenchmarkCase const& bench,
                    fs::path const& root_path,
                    std::string const& index_name,
                    std::uint64_t n_events)
    {
      std::string discard;
      (void)read_once(bench, root_path, index_name, n_events, 0, discard);
    }

  }

  void run_read_benchmark(BenchmarkCase const& bench,
                          fs::path const& run_dir,
                          std::ostream& summary_csv)
  {
    nlohmann::json manifest = load_json(bench.manifest_file);
    std::string index_name = index_name_from_manifest(manifest);
    fs::path root_path = bench.root_file;
    std::vector<fs::path> container_paths{root_path};

    std::map<std::string, fgs::bench::ContainerFacts> dataset_facts;
    std::uint64_t const n_events = [&] {
      ROOT::RNTupleReadOptions probe_opts;
      probe_opts.SetClusterCache(ROOT::RNTupleReadOptions::EClusterCache::kOff);
      fgs::EventReader probe(root_path, index_name, probe_opts, bench.products);
      for (auto const& [name, facts] : probe.dataset_facts())
        dataset_facts[name] = {facts.clusters, facts.pages};
      return std::min(bench.num_events, probe.num_events());
    }();

    // dataset_facts is keyed by the containers the reader opened, which is
    // exactly the selection.
    std::string containers;
    for (auto const& [name, facts] : dataset_facts)
      containers += containers.empty() ? name : "|" + name;

    // One self-contained folder per benchmark, under the run's benchmarks/ dir.
    fs::path const bench_dir =
      run_dir / "benchmarks" / ("benchmark_" + std::to_string(bench.benchmark_num));
    fs::create_directories(bench_dir / "csv");
    fs::create_directories(bench_dir / "runs");

    // A throwaway order built outside every timed region, only so the realized
    // displacement can be reported next to the requested distance. Other
    // patterns report distance 0, so the column never carries a value that was
    // parsed but not applied.
    bool const is_scatter = bench.access_pattern == "scatter";
    OrderStats const scatter_stats =
      is_scatter ? make_event_order(order_spec(bench, n_events))->stats() : OrderStats{};
    std::uint64_t const scatter_distance = is_scatter ? bench.scatter_distance : 0;

    fgs::bench::BenchmarkId id{bench.benchmark_num,
                               bench.name,
                               bench.description,
                               bench.variant,
                               bench.access_pattern,
                               scatter_distance,
                               cache_state_name(bench.cache_state),
                               bench.cluster_cache,
                               bench.implicit_mt,
                               containers,
                               n_events,
                               bench.repetitions,
                               bench.root_file.string(),
                               bench.manifest_file.string(),
                               manifest.value("write_options_effective", nlohmann::json::object())
                                 .value("max_unzipped_page_size_bytes", std::uint64_t{0}),
                               is_scatter ? bench.scatter_seed : std::uint64_t{0},
                               scatter_stats.mean_displacement,
                               scatter_stats.max_displacement};
    fgs::bench::write_benchmark_metadata(bench_dir / "metadata.txt", id, dataset_facts);

    std::ofstream log(bench_dir / "benchmark.log");
    print_benchmark_header(log, bench, root_path, n_events, containers);

    std::vector<Measurement> reps;
    reps.reserve(bench.repetitions);
    for (std::uint64_t rep = 1; rep <= bench.repetitions; ++rep) {
      if (bench.cache_state == CacheState::Cold) {
        for (fs::path const& path : container_paths)
          evict_from_cache(path);
      } else if (bench.warmup) {
        warm_cache(bench, root_path, index_name, n_events);
      }

      std::string const started = fgs::bench::timestamp_human();
      std::string metrics_raw;
      Measurement m = read_once(bench, root_path, index_name, n_events, rep, metrics_raw);
      m.counters = fgs::bench::parse_read_counters(metrics_raw);

      std::ostringstream line;
      line << "rep " << rep << " wall_s=" << m.wall_s
           << " latency_us_per_event=" << m.latency_us_per_event
           << " throughput_evt_s=" << m.throughput_evt_s << " elements=" << m.total_elements
           << " read_ms=" << m.counters.read_wall_ms << " unzip_ms=" << m.counters.unzip_wall_ms
           << " n_read=" << m.counters.n_read << "\n\n";
      tee(log, line.str());

      fgs::bench::write_run_report(
        bench_dir / "runs" / ("run_" + std::to_string(rep) + ".txt"), id, m, started, metrics_raw);
      reps.push_back(m);
    }

    std::uint64_t dataset_pages = 0;
    for (auto const& [name, facts] : dataset_facts)
      dataset_pages += facts.pages;

    fgs::bench::write_raw_csv(bench_dir / "csv" / "raw.csv", id, reps);
    fgs::bench::write_benchmark_summary_txt(bench_dir / "summary.txt", id, reps, dataset_pages);
    fgs::bench::append_summary_row(summary_csv, id, reps, dataset_pages);
  }

}
