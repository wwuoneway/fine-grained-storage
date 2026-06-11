#include "fgs/config.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace fgs {

  namespace {

    // Throw a uniformly-formatted validation error.
    [[noreturn]] void fail(std::string const& msg)
    {
      throw std::runtime_error("config error: " + msg);
    }

  }

  Config load_config(std::filesystem::path const& path)
  {
    std::ifstream in(path);
    if (!in)
      fail("cannot open config file: " + path.string());

    // Parse. nlohmann::json throws on syntax errors; translate to our message.
    nlohmann::json j;
    try {
      in >> j;
    } catch (nlohmann::json::exception const& e) {
      fail("invalid JSON in " + path.string() + ": " + e.what());
    }

    Config c;
    try {
      c.num_events = j.at("num_events").get<std::uint64_t>();
      c.seed = j.at("seed").get<std::uint64_t>();
      c.output_dir = j.at("output_dir").get<std::string>();

      c.particles.min = j.at("particles").at("min").get<std::uint32_t>();
      c.particles.max = j.at("particles").at("max").get<std::uint32_t>();

      c.position.min = j.at("position").at("min").get<double>();
      c.position.max = j.at("position").at("max").get<double>();

      c.momentum.mean = j.at("momentum").at("mean").get<double>();
      c.momentum.stddev = j.at("momentum").at("stddev").get<double>();
      c.momentum.min_magnitude = j.at("momentum").at("min_magnitude").get<double>();
    } catch (nlohmann::json::exception const& e) {
      fail(std::string("missing/wrong-typed field: ") + e.what());
    }

    //Validation
    if (c.num_events == 0)
      fail("num_events must be > 0");
    if (c.particles.min > c.particles.max)
      fail("particles.min must be <= particles.max");
    if (c.position.min > c.position.max)
      fail("position.min must be <= position.max");
    if (!(c.momentum.stddev > 0.0))
      fail("momentum.stddev must be > 0");
    if (c.momentum.min_magnitude < 0.0)
      fail("momentum.min_magnitude must be >= 0");

    return c;
  }

}
