// fgs_read: demo driver for EventReader. Resolves a variant's ROOT file from the
// writing manifest (this is the "upper level" that knows about variants), then
// reads and prints the requested events through one EventReader.
//
// Usage: fgs_read [strategy_root] [variant] [options]
//   strategy_root   default: output/writing/rntuple/strategy_one
//   variant         default: no-shuffle
//   --events a,b,c  event ids to read (default: event 0)
//   --product NAME  product to read (e.g. position, momentum)
//   --field NAME    a single field of --product (e.g. x, pz)
//   --metrics       enable ROOT read metrics and print them at the end
//
// The read mode is chosen by which options you pass:
//   (no --product/--field)   full event: position + momentum joined (read_event)
//   --product P              whole-row read of product P, all columns (read_product)
//   --product P --field F    single field F of product P, column projection (read_field)
//
// Examples:
//   fgs_read                                            # event 0, full event
//   fgs_read output/writing/rntuple/strategy_one shuffle --events 0,5,42
//   fgs_read --product position --events 0,5            # whole position rows
//   fgs_read --product momentum --field pz --events 0   # just momentum.pz

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "event_reader.hpp"
#include "fgs/bin_io.hpp"

namespace fs = std::filesystem;

namespace {

  // Read the product registry (name + numeric index id) and the index TTree name
  // from the writing manifest. The reader itself never touches the manifest.
  std::vector<fgs::ProductSpec> products_from_manifest(nlohmann::json const& manifest,
                                                       std::string& index_name)
  {
    std::vector<fgs::ProductSpec> products;
    for (auto const& p : manifest.at("products"))
      products.push_back(
        {p.at("name").get<std::string>(), p.at("product_id").get<std::uint64_t>()});
    index_name = manifest.at("products").at(0).at("index_container").get<std::string>();
    return products;
  }

  // Join the "position" and "momentum" products for one event into an fgs::Event.
  // Lives here, not in EventReader, because EventReader is product-agnostic.
  fgs::Event read_event(fgs::EventReader& reader, std::uint64_t event_id)
  {
    std::vector<float> pos = reader.read_product(event_id, "position");
    std::vector<float> mom = reader.read_product(event_id, "momentum");
    if (pos.size() != mom.size())
      throw std::runtime_error("position/momentum particle counts differ at event " +
                               std::to_string(event_id));
    constexpr std::size_t kComponents = 3;
    auto const n_particles = static_cast<std::uint32_t>(pos.size() / kComponents);
    return fgs::reconstruct_event(event_id, n_particles, pos, mom);
  }

  // Resolve a variant's ROOT file (relative to strategy_root) from the manifest.
  fs::path variant_file(nlohmann::json const& manifest, std::string const& variant)
  {
    for (auto const& v : manifest.at("variants"))
      if (v.at("name").get<std::string>() == variant)
        return v.at("file").get<std::string>();
    throw std::runtime_error("variant \"" + variant + "\" not found in manifest");
  }

}

int main(int argc, char** argv)
{
  std::vector<std::uint64_t> user_events;
  std::vector<std::string> positional;
  bool metrics = false;
  std::string product; // empty -> full-event mode
  std::string field;   // set -> single-field mode (needs product)
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--metrics") {
      metrics = true;
    } else if (a == "--events") {
      if (i + 1 >= argc) {
        std::cerr << "fgs_read: --events requires a comma-separated list\n";
        return 1;
      }
      std::stringstream ss(argv[++i]);
      std::string tok;
      while (std::getline(ss, tok, ','))
        if (!tok.empty())
          user_events.push_back(std::stoull(tok));
    } else if (a == "--product") {
      if (i + 1 >= argc) {
        std::cerr << "fgs_read: --product requires a name\n";
        return 1;
      }
      product = argv[++i];
    } else if (a == "--field") {
      if (i + 1 >= argc) {
        std::cerr << "fgs_read: --field requires a name\n";
        return 1;
      }
      field = argv[++i];
    } else {
      positional.push_back(a);
    }
  }

  if (!field.empty() && product.empty()) {
    std::cerr << "fgs_read: --field also needs --product (which product's field?)\n";
    return 1;
  }

  fs::path strat_root = positional.size() > 0 ? fs::path{positional[0]}
                                              : fs::path{"output/writing/rntuple/strategy_one"};
  std::string variant = positional.size() > 1 ? positional[1] : "no-shuffle";

  try {
    nlohmann::json manifest;
    {
      std::ifstream in(strat_root / "manifest.json");
      if (!in)
        throw std::runtime_error("cannot open " + (strat_root / "manifest.json").string());
      in >> manifest;
    }

    std::string index_name;
    std::vector<fgs::ProductSpec> products = products_from_manifest(manifest, index_name);
    fs::path root_path = strat_root / variant_file(manifest, variant);

    ROOT::RNTupleReadOptions opts;
    opts.SetEnableMetrics(metrics);

    fgs::EventReader reader(root_path, index_name, products, opts);
    std::cout << "opened " << root_path << " (" << reader.num_events() << " events)\n";

    if (user_events.empty())
      user_events.push_back(0); // demo default: just the first event

    for (std::uint64_t id : user_events) {
      if (!field.empty()) {
        // single-field mode: one column of one product (column projection)
        std::vector<float> values = reader.read_field(id, product, field);
        std::cout << "event " << id << ": " << product << "." << field << " =";
        for (float v : values)
          std::cout << ' ' << v;
        std::cout << "  (" << values.size() << " values)\n";
      } else if (!product.empty()) {
        // whole-row mode: every column of one product
        std::vector<float> floats = reader.read_product(id, product);
        std::cout << "event " << id << ": " << product << " = " << floats.size() << " floats:";
        for (float v : floats)
          std::cout << ' ' << v;
        std::cout << '\n';
      } else {
        // full-event mode: position + momentum joined
        fgs::Event ev = read_event(reader, id);
        std::cout << "event " << id << ": " << ev.n_particles() << " particles\n";
        for (std::size_t i = 0; i < ev.positions.size(); ++i) {
          fgs::Position const& p = ev.positions[i];
          fgs::Momentum const& m = ev.momenta[i];
          std::cout << "  [" << i << "] pos=(" << p.x << ", " << p.y << ", " << p.z
                    << ") mom=(" << m.px << ", " << m.py << ", " << m.pz << ")\n";
        }
      }
    }

    if (metrics)
      reader.print_metrics();

    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fgs_read: error: " << e.what() << "\n";
    return 1;
  }
}
