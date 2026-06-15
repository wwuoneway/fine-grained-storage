#include "fgs/generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "fgs/bin_io.hpp"

namespace fgs {

  namespace {

    constexpr int kMaxMomentumResamples = 1000;

  }

  Event generate_event(Config const& cfg, std::uint64_t event_id, std::mt19937_64& rng)
  {
    std::uniform_int_distribution<std::uint32_t> n_dist(cfg.particles.min, cfg.particles.max);
    std::uniform_real_distribution<float> x_dist(static_cast<float>(cfg.position.x.min),
                                                  static_cast<float>(cfg.position.x.max));
    std::uniform_real_distribution<float> y_dist(static_cast<float>(cfg.position.y.min),
                                                  static_cast<float>(cfg.position.y.max));
    std::uniform_real_distribution<float> z_dist(static_cast<float>(cfg.position.z.min),
                                                  static_cast<float>(cfg.position.z.max));
    std::normal_distribution<float> mom_dist(static_cast<float>(cfg.momentum.mean),
                                             static_cast<float>(cfg.momentum.stddev));

    float const min_mag = static_cast<float>(cfg.momentum.min_magnitude);

    Event event;
    event.id = event_id;

    std::uint32_t const n = n_dist(rng);
    event.positions.resize(n);
    event.momenta.resize(n);

    for (std::uint32_t i = 0; i < n; ++i) {
      event.positions[i] = Position{x_dist(rng), y_dist(rng), z_dist(rng)};
    }

    for (std::uint32_t i = 0; i < n; ++i) {
      int attempts = 0;
      while (true) {
        float const px = mom_dist(rng);
        float const py = mom_dist(rng);
        float const pz = mom_dist(rng);
        float const mag = std::sqrt(px * px + py * py + pz * pz);
        if (mag >= min_mag) {
          event.momenta[i] = Momentum{px, py, pz};
          break;
        }
        if (++attempts >= kMaxMomentumResamples) {
          throw std::runtime_error("generate_event: exceeded " +
                                   std::to_string(kMaxMomentumResamples) +
                                   " momentum resample attempts (min_magnitude too large for the "
                                   "configured Gaussian?)");
        }
      }
    }

    return event;
  }

  namespace {

    nlohmann::json config_to_json(Config const& cfg)
    {
      return nlohmann::json{
        {"num_events", cfg.num_events},
        {"particles", {{"min", cfg.particles.min}, {"max", cfg.particles.max}}},
        {"seed", cfg.seed},
        {"output_dir", cfg.output_dir},
        {"position",
         {{"distribution", cfg.position.distribution},
          {"x", {{"min", cfg.position.x.min}, {"max", cfg.position.x.max}}},
          {"y", {{"min", cfg.position.y.min}, {"max", cfg.position.y.max}}},
          {"z", {{"min", cfg.position.z.min}, {"max", cfg.position.z.max}}}}},
        {"momentum",
         {{"mean", cfg.momentum.mean},
          {"stddev", cfg.momentum.stddev},
          {"min_magnitude", cfg.momentum.min_magnitude}}},
      };
    }

  } // namespace

  RunStats run_generation(Config const& cfg)
  {
    namespace fs = std::filesystem;

    fs::path const output_dir = cfg.output_dir;
    fs::create_directories(output_dir);

    // Remove stale product files so a re-run never leaves oversized old data.
    fs::remove(output_dir / "positions.bin");
    fs::remove(output_dir / "momenta.bin");

    nlohmann::json events_array = nlohmann::json::array();

    RunStats stats;
    stats.output_dir = output_dir;
    stats.manifest_path = output_dir / "manifest.json";
    stats.num_events = cfg.num_events;

    {
      // Open both product files before the loop so headers are written once.
      // Destructors flush and close before fs::file_size is called below.
      ProductWriter pos_writer(output_dir / "positions.bin", "position", "float3", cfg.num_events);
      ProductWriter mom_writer(output_dir / "momenta.bin", "momentum", "float3", cfg.num_events);

      std::mt19937_64 rng(cfg.seed);

      for (std::uint64_t id = 0; id < cfg.num_events; ++id) {
        Event const event = generate_event(cfg, id, rng);
        std::uint32_t const n = static_cast<std::uint32_t>(event.n_particles());

        pos_writer.write_event(n, reinterpret_cast<float const*>(event.positions.data()));
        mom_writer.write_event(n, reinterpret_cast<float const*>(event.momenta.data()));

        stats.total_particles += n;
        if (id == 0) {
          stats.min_particles = n;
          stats.max_particles = n;
        } else {
          stats.min_particles = std::min(stats.min_particles, n);
          stats.max_particles = std::max(stats.max_particles, n);
        }

        events_array.push_back({{"event_id", id}, {"n_particles", n}});
      }
    } // ProductWriters flush and close here

    stats.positions_bytes = static_cast<std::uint64_t>(fs::file_size(output_dir / "positions.bin"));
    stats.momenta_bytes = static_cast<std::uint64_t>(fs::file_size(output_dir / "momenta.bin"));
    stats.total_bytes = stats.positions_bytes + stats.momenta_bytes;

    auto mb = [](std::uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };

    // ordered_json preserves insertion order so the summary block appears at the
    // top of the file — easy to scan without scrolling past config or events.
    nlohmann::ordered_json manifest;
    manifest["format"] = "FGS2";
    manifest["num_events"] = stats.num_events;
    manifest["total_particles"] = stats.total_particles;
    manifest["positions_bytes"] = stats.positions_bytes;
    manifest["positions_mb"] = mb(stats.positions_bytes);
    manifest["momenta_bytes"] = stats.momenta_bytes;
    manifest["momenta_mb"] = mb(stats.momenta_bytes);
    manifest["total_bytes"] = stats.total_bytes;
    manifest["total_mb"] = mb(stats.total_bytes);
    manifest["positions_file"] = "positions.bin";
    manifest["momenta_file"] = "momenta.bin";
    manifest["seed"] = cfg.seed;
    manifest["config"] = config_to_json(cfg);
    manifest["events"] = std::move(events_array);

    std::ofstream mout(stats.manifest_path);
    if (!mout)
      throw std::runtime_error("run_generation: cannot open " + stats.manifest_path.string());
    mout << manifest.dump(2) << '\n';
    if (!mout)
      throw std::runtime_error("run_generation: failed writing manifest");

    return stats;
  }

}
