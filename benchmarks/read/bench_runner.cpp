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
#include "fgs/event_reader.hpp"
#include "os_cache.hpp"

namespace fs = std::filesystem;

namespace fgs::bench {

  namespace {

    std::string index_name_from_manifest(nlohmann::json const& manifest)
    {
      return manifest.at("products").at(0).at("index_container").get<std::string>();
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
                          std::uint64_t n_events,
                          std::uint64_t repetition,
                          std::string& metrics_raw,
                          bool instrument = false)
    {
      ROOT::RNTupleReadOptions opts = make_read_options(bench);
      fgs::EventReader reader(root_path, index_name, opts);
      reader.set_instrument(instrument);

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
      // in [0, n_events) exactly once), reading every product the file holds.
      std::vector<std::string> const& products = reader.product_names();
      auto const start = std::chrono::steady_clock::now();
      for (std::uint64_t i = 0; i < n_events; ++i) {
        std::uint64_t const event_id = order->next();
        for (std::string const& product : products) {
          std::vector<float> values = reader.read_product(event_id, product);
          total_values += values.size();
        }
      }
      auto const stop = std::chrono::steady_clock::now();

      // Keep the timer focused on the read loop
      m.wall_s = std::chrono::duration<double>(stop - start).count();
      m.latency_us_per_event = m.wall_s / static_cast<double>(n_events) * 1.0e6;
      m.throughput_evt_s = m.wall_s > 0.0 ? static_cast<double>(n_events) / m.wall_s : 0.0;
      m.total_values = total_values;

      std::ostringstream raw;
      reader.print_metrics(raw);
      metrics_raw = raw.str();

      if (instrument) {
        auto const& st = reader.subtimers();
        m.locate_ms = st.locate_ns / 1e6;
        m.load_ms = st.load_ns / 1e6;
        m.fill_ms = st.fill_ns / 1e6;
      }

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

    std::uint64_t const n_events = [&] {
      ROOT::RNTupleReadOptions probe_opts;
      probe_opts.SetClusterCache(ROOT::RNTupleReadOptions::EClusterCache::kOff);
      fgs::EventReader probe(root_path, index_name, probe_opts);
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
        warm_cache(bench, root_path, index_name, n_events);
      }

      std::string const started = fgs::bench::timestamp_human();
      std::string metrics_raw;
      Measurement m = read_once(bench, root_path, index_name, n_events, rep, metrics_raw);
      m.counters = fgs::bench::parse_read_counters(metrics_raw);

      // Sub-timers come from a second pass so the in-loop clocks stay out of the
      // wall above. Re-evict on cold so it reads at the same cache state, else
      // decode = load - io - unzip goes negative (warm load vs cold io).
      if (bench.cache_state == CacheState::Cold)
        for (fs::path const& path : container_paths)
          evict_from_cache(path);
      std::string discard;
      Measurement instr = read_once(bench, root_path, index_name, n_events, rep, discard, true);
      m.locate_ms = instr.locate_ms;
      m.load_ms = instr.load_ms;
      m.fill_ms = instr.fill_ms;
      m.wall_instr_ms = instr.wall_s * 1000.0;

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
