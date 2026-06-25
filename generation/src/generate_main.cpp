// Usage:
//   fgs_generate [path/to/config.json]
//
// With no argument it uses configs/generation/default.json (resolved relative to the current
// working directory) run it from the repo root.

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "fgs/config.hpp"
#include "fgs/generator.hpp"

int main(int argc, char** argv)
{
  // One optional positional argument: the config path. argv[0] is the program
  // name, so a real argument is at argv[1].
  std::filesystem::path const config_path =
    (argc > 1) ? std::filesystem::path{argv[1]}
               : std::filesystem::path{"configs/generation/default.json"};

  if (argc > 2) {
    std::cerr << "usage: " << argv[0] << " [config.json]\n";
    return EXIT_FAILURE;
  }

  try {
    fgs::Config const cfg = fgs::load_config(config_path);
    fgs::RunStats const stats = fgs::run_generation(cfg);

    // --- stdout summary ------------------------------------------------------
    double const avg_particles = stats.num_events ? static_cast<double>(stats.total_particles) /
                                                      static_cast<double>(stats.num_events)
                                                  : 0.0;

    auto fmt_mb = [](std::uint64_t b) -> std::string {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << static_cast<double>(b) / (1024.0 * 1024.0)
          << " MB";
      return oss.str();
    };

    std::cout << "fgs_generate: generation complete\n"
              << "  events          : " << stats.num_events << "\n"
              << "  total particles : " << stats.total_particles << "\n"
              << "  particles/event : min " << stats.min_particles << ", avg " << std::fixed
              << std::setprecision(2) << avg_particles << ", max " << stats.max_particles << "\n"
              << "  positions.bin   : " << stats.positions_bytes << " bytes ("
              << fmt_mb(stats.positions_bytes) << ")\n"
              << "  momenta.bin     : " << stats.momenta_bytes << " bytes ("
              << fmt_mb(stats.momenta_bytes) << ")\n"
              << "  total           : " << stats.total_bytes << " bytes ("
              << fmt_mb(stats.total_bytes) << ")\n"
              << "  config file     : " << config_path.string() << "\n"
              << "  output location : " << stats.output_dir.string() << "\n"
              << "  manifest        : " << stats.manifest_path.string() << "\n";

    return EXIT_SUCCESS;
  } catch (std::exception const& e) {
    // Any config or I/O failure lands here with a clear message.
    std::cerr << "fgs_generate: error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
