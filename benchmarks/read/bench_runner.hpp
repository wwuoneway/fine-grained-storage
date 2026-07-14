#pragma once

// Execution of a single benchmark case: resolve the event count, drive the timed
// read repetitions (cold eviction or warmup as configured), and emit the per-case
// reports plus the aggregated summary row.

#include <filesystem>
#include <iosfwd>

#include "bench_config.hpp"

namespace fgs::bench {

  // Run one benchmark case, writing its self-contained folder under run_dir and
  // appending one aggregated row to summary_csv.
  void run_read_benchmark(BenchmarkCase const& bench,
                          std::filesystem::path const& run_dir,
                          std::ostream& summary_csv);

}
