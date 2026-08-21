// Read benchmark entry point: load the benchmark config, set up the run folder
// the caller named, and run each enabled case serially. All measurement lives
// in bench_runner; this file is orchestration only.

#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "bench_config.hpp"
#include "bench_report.hpp"
#include "bench_runner.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "usage: fgs_read_bench <config.json> <run_dir>\n";
    return 1;
  }
  fs::path const config_path{argv[1]};

  try {
    nlohmann::json config = fgs::bench::load_json(config_path);

    // The run root holds the shared run_info.json and summary.csv; each
    // benchmark gets its own self-contained subfolder beneath it.
    fs::path const run_dir{argv[2]};
    fs::create_directories(run_dir);

    std::ofstream summary_csv(run_dir / "summary.csv");
    summary_csv << fgs::bench::summary_header();
    fgs::bench::write_run_info(run_dir / "run_info.json", config_path, config);

    int total_enabled = 0;
    for (auto const& item : config.at("benchmarks"))
      if (item.value("enabled", true))
        ++total_enabled;

    int index = 0;
    int done = 0;
    int failures = 0;
    for (auto const& item : config.at("benchmarks")) {
      ++index;
      fgs::bench::BenchmarkCase bench = fgs::bench::parse_benchmark(item);
      if (bench.benchmark_num == 0)
        bench.benchmark_num = index; // default to config order when unset
      if (!bench.enabled)
        continue;
      ++done;
      std::cout << "\nrunning benchmark " << done << "/" << total_enabled << ": " << bench.name
                << "\n";
      try {
        fgs::bench::run_read_benchmark(bench, run_dir, summary_csv);
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
