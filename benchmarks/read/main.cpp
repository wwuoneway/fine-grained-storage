// Read benchmark entry point: load the benchmark config, set up a fresh run
// folder, and run each enabled case serially. All measurement lives in
// bench_runner; this file is orchestration only.

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
  fs::path config_path =
    argc > 1 ? fs::path{argv[1]} : fs::path{"configs/benchmarks/reading_benchmarks.json"};

  try {
    nlohmann::json config = fgs::bench::load_json(config_path);

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
      fgs::bench::BenchmarkCase bench = fgs::bench::parse_benchmark(item);
      if (bench.benchmark_num == 0)
        bench.benchmark_num = index; // default to config order when unset
      if (!bench.enabled)
        continue;
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
