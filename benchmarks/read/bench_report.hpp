#pragma once

// Reporting helpers for the read benchmark: gathering the machine description,
// writing the run record and CSVs, and turning ROOT's raw metrics dump into a
// readable table. Kept out of fgs_read_bench.cpp so the benchmark logic there
// stays uncluttered by file-formatting boilerplate.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace fgs::bench {

  // One timed pass over the events.
  struct Measurement {
    std::uint64_t repetition = 0;
    double wall_s = 0.0;
    double latency_us_per_event = 0.0;
    double throughput_evt_s = 0.0;
    std::uint64_t total_values = 0;
  };

  // A benchmark's identity and configuration, used across the CSV rows and the
  // human-readable per-benchmark reports.
  struct BenchmarkId {
    int num = 0;
    std::string name;
    std::string description;
    std::string variant;
    std::string access_pattern;
    std::string cache_state;
    std::string cluster_cache;
    std::uint64_t num_events = 0;
    std::uint64_t repetitions = 0;
    std::string root_file;
    std::string manifest_file;
  };

  std::string timestamp_now();   // YYYYmmdd-HHMMSS (for run-directory names)
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

  // Human-readable per-benchmark files.
  void write_benchmark_metadata(std::filesystem::path const& path, BenchmarkId const& id);
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
