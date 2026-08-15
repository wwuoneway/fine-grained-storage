#pragma once

// Reporting helpers for the read benchmark: gathering the machine description,
// writing the run record and CSVs, and turning ROOT's raw metrics dump into a
// readable table.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace fgs::bench {

  // Mirrors fgs::EventReader::ContainerFacts without depending on event_reader.hpp.
  struct ContainerFacts {
    std::uint64_t clusters = 0;
    std::uint64_t pages = 0;
  };

  // The handful of ROOT RNTuple performance counters worth carrying into the CSV,
  // summed across the event's products (position + momentum). These attribute read
  // cost to its layers: storage I/O vs decompression, and read amplification.
  struct ReadCounters {
    double read_wall_ms = 0.0;    // timeWallRead: wall time in storage I/O
    double unzip_wall_ms = 0.0;   // timeWallUnzip: wall time decompressing
    double read_payload_mib = 0.0; // szReadPayload: bytes pulled from storage
    std::uint64_t n_read = 0;     // nRead: number of byte-range reads (seeks)
    double read_efficiency = std::numeric_limits<double>::quiet_NaN(); // payload / (payload + overhead)
    std::uint64_t n_page_read = 0;      // nPageRead: sealed pages fetched from storage
    std::uint64_t n_page_unsealed = 0;  // nPageUnsealed: pages actually decompressed
    std::uint64_t n_cluster_loaded = 0; // nClusterLoaded: clusters fetched from storage
  };

  // Parse ROOT's pipe-delimited kMetrics dump (as captured by EventReader::
  // print_metrics, one section per product) and sum the counters above. Empty
  // dump -> all-default counters. Ratios are recomputed from the summed volumes.
  ReadCounters parse_read_counters(std::string const& raw_dump);

  // One timed pass over the events.
  struct Measurement {
    std::uint64_t repetition = 0;
    double wall_s = 0.0;
    double latency_us_per_event = 0.0;
    double throughput_evt_s = 0.0;
    std::uint64_t total_values = 0;
    // ROOT RNTuple counters for this pass (only when metrics are enabled).
    ReadCounters counters;
    // "Other"-segment breakdown from the instrumented pass (ms).
    double locate_ms = 0.0;
    double load_ms = 0.0;
    double fill_ms = 0.0;
    // Wall of the instrumented pass itself: the sub-timers are nested inside
    // this wall (not wall_s, which is a different execution), so residuals
    // against it stay non-negative.
    double wall_instr_ms = 0.0;
    // ROOT counters from the instrumented pass itself: the fine bottleneck
    // split (decode = load - read - unzip) must subtract counters measured on
    // the same execution as load_ms, not the clean pass's.
    double read_wall_instr_ms = 0.0;
    double unzip_wall_instr_ms = 0.0;
  };

  // A benchmark's identity and configuration, used across the CSV rows and the
  // human-readable per-benchmark reports.
  struct BenchmarkId {
    int num = 0;
    std::string name;
    std::string description;
    std::string variant;
    std::string access_pattern;
    std::uint64_t scatter_distance = 0;
    std::string cache_state;
    std::string cluster_cache;
    std::string implicit_mt;
    std::string containers; // the containers read, names joined with '|'
    std::uint64_t num_events = 0;
    std::uint64_t repetitions = 0;
    std::string root_file;
    std::string manifest_file;
    std::uint64_t max_page_size_bytes = 0;
    std::uint64_t scatter_seed = 0;
    // Realized displacement of the scatter order, zero for every other pattern.
    double scatter_mean_displacement = 0.0;
    std::uint64_t scatter_max_displacement = 0;
  };

  std::string timestamp_now();   // YYYYmmdd-HHMMSS
  std::string timestamp_human(); // YYYY-MM-DD HH:MM:SS (for readable reports)

  // Machine description (CPU, cores, RAM, kernel, ROOT version) as JSON.
  nlohmann::json system_info();

  // Write run_info.json: the config that was run plus the machine it ran on, so
  // a result folder is self-describing.
  void write_run_info(std::filesystem::path const& path,
                      std::filesystem::path const& config_file,
                      nlohmann::json const& config);

  // Header row for summary.csv (one aggregated row per benchmark).
  char const* summary_header();

  // Write a benchmark's per-repetition raw CSV (header + one row per rep).
  void write_raw_csv(std::filesystem::path const& path,
                     BenchmarkId const& id,
                     std::vector<Measurement> const& reps);

  // Append one aggregated (mean/min) row for a benchmark to summary.csv.
  void append_summary_row(std::ostream& csv,
                          BenchmarkId const& id,
                          std::vector<Measurement> const& reps);

  // Human-readable per-benchmark files. `dataset_facts` is keyed by container
  // name; empty when not available (e.g. facts weren't collected).
  void write_benchmark_metadata(std::filesystem::path const& path, BenchmarkId const& id,
                                std::map<std::string, ContainerFacts> const& dataset_facts = {});
  void write_benchmark_summary_txt(std::filesystem::path const& path,
                                   BenchmarkId const& id,
                                   std::vector<Measurement> const& reps);

  // Reformat ROOT's pipe-delimited kMetrics dump into an aligned table string
  // (empty in -> empty out).
  std::string format_metrics_table(std::string const& raw_dump);

  // Write one repetition's report: brief metadata + measurement, then the metrics
  // table (if metrics_raw is non-empty).
  void write_run_report(std::filesystem::path const& path,
                        BenchmarkId const& id,
                        Measurement const& m,
                        std::string const& started_at,
                        std::string const& metrics_raw);

}
