// fgs_strategy_one: write strategy "strategy_one" -- two data-product RNTuples
// (position, momentum) plus a shared TTree index, both inside one ROOT file.
//
// Each product is one RNTuple container holding one row per event; a row is the
// event's whole particle collection (a vector<Position> or vector<Momentum>).
// The index TTree has two columns, event_id and index_value: for each event,
// index_value maps a product name to its Token (container + entry), so a reader
// locates an event's row from the index without assuming any physical layout.
//
// It emits one output folder per write variant under the strategy's output root:
//   <output_root>/no-shuffle/strategy_one.root   (events written in event order)
//   <output_root>/shuffle/strategy_one_shuffled.root  (events written shuffled)
// Each folder also carries its own manifest.json. The shuffle changes only the
// physical row layout -- the index makes reads order-independent either way.
//
// Usage: fgs_strategy_one [config.json]   (default: configs/writing/strategy_one.json)

#include <ROOT/RNTupleInspector.hxx>
#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleWriteOptions.hxx>
#include <ROOT/RNTupleWriter.hxx>
#include <TFile.h>
#include <TTree.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "fgs/bin_io.hpp"
#include "fgs/ordering.hpp"
#include "fgs/token.hpp"
#include "fgs/types.hpp"
#include "fgs/util.hpp"

namespace fs = std::filesystem;

namespace {
  using json = nlohmann::ordered_json;

  // Components per particle in a product's flat float buffer (x,y,z / px,py,pz).
  constexpr std::uint64_t kComponents = 3;

  std::string const kPositionContainer = "position_container";
  std::string const kMomentumContainer = "momentum_container";

  constexpr double kBytesPerMiB = 1024.0 * 1024.0;

  double bytes_to_mib(std::uint64_t bytes) { return fgs::round3(static_cast<double>(bytes) / kBytesPerMiB); }

  // Cluster and page counts of one written RNTuple container, read back from its
  // descriptor. These are finalized only once the writer has committed, so this
  // reopens the closed file rather than querying the writer.
  struct Layout {
    std::uint64_t clusters = 0;
    std::uint64_t pages = 0;
  };

  Layout inspect_layout(std::string const& container, fs::path const& root_path)
  {
    auto insp = ROOT::Experimental::RNTupleInspector::Create(container, root_path.string());
    ROOT::RNTupleDescriptor const& desc = insp->GetDescriptor();
    Layout l;
    l.clusters = desc.GetNClusters();
    for (std::size_t col = 0; col < desc.GetNPhysicalColumns(); ++col)
      l.pages += insp->GetColumnInspector(static_cast<ROOT::DescriptorId_t>(col)).GetNPages();
    return l;
  }

  // MiB, not MB: ROOT's write options are byte counts against power-of-two
  // defaults (MaxUnzippedPageSize is 1 MiB), so 10^6 would never line up.
  std::size_t mib_to_bytes(double mib, std::string const& key)
  {
    if (!(mib > 0.0))
      throw std::runtime_error("write_options." + key + " must be greater than 0");
    return static_cast<std::size_t>(std::llround(mib * kBytesPerMiB));
  }

  struct StrategyConfig {
    fs::path gen_dir;
    fs::path output_root;
    std::vector<std::string> variants;
    std::uint64_t shuffle_seed = 0;
    // Kept alongside the ROOT object below because run_study_all.sh diffs this
    // block against the config to decide whether a rewrite is needed, and
    // recovering MiB from ROOT's rounded byte count would not survive that.
    json write_options_json = json::object();
    ROOT::RNTupleWriteOptions write_options;
  };

  StrategyConfig load_strategy_config(fs::path const& path)
  {
    std::ifstream in(path);
    if (!in)
      throw std::runtime_error("cannot open config: " + path.string());

    json j;
    in >> j;

    StrategyConfig cfg;
    cfg.gen_dir = j.at("gen_dir").get<std::string>();
    cfg.output_root = j.at("output_root").get<std::string>();
    cfg.variants = j.at("variants").get<std::vector<std::string>>();
    cfg.shuffle_seed = j.at("shuffle_seed").get<std::uint64_t>();

    if (auto it = j.find("write_options"); it != j.end()) {
      cfg.write_options_json = *it;
      for (auto const& [key, value] : it->items()) {
        if (key == "max_page_size_mib")
          cfg.write_options.SetMaxUnzippedPageSize(mib_to_bytes(value.get<double>(), key));
        else
          // Ignoring it would label the run with a layout it was never written in.
          throw std::runtime_error("write_options: unsupported key '" + key + "'");
      }
    }
    return cfg;
  }

  struct VariantResult {
    std::string name;
    std::string file_name;
    std::uint64_t bytes;
    std::vector<std::pair<std::string, Layout>> containers;
  };

  // Distinct file name so the two outputs are never confused side by side.
  std::string root_file_for(std::string const& variant)
  {
    return variant == "shuffle" ? "strategy_one_shuffled.root" : "strategy_one.root";
  }

  // One manifest per strategy: shared product registry plus every variant's
  // file (relative to the strategy root), size, and seed (shuffle only).
  void write_manifest(fs::path const& path,
                      std::uint64_t num_events,
                      std::uint64_t total_particles,
                      std::uint64_t min_particles,
                      std::uint64_t max_particles,
                      std::vector<VariantResult> const& variants,
                      std::uint64_t shuffle_seed,
                      json const& write_options_json,
                      ROOT::RNTupleWriteOptions const& write_options)
  {
    struct ProductDesc {
      char const* name;
      char const* container;
    };
    constexpr ProductDesc products[] = {{"position", "position_container"},
                                        {"momentum", "momentum_container"}};

    // Mean on-disk footprint of one event, averaged over the variant files (each
    // variant stores the same events, so their sizes differ only by row order).
    std::uint64_t total_bytes = 0;
    for (VariantResult const& v : variants)
      total_bytes += v.bytes;
    double const mean_file_bytes =
      variants.empty() ? 0.0 : static_cast<double>(total_bytes) / variants.size();

    json manifest;
    manifest["generated_at"] = fgs::utc_timestamp();
    manifest["strategy"] = "strategy_one";
    manifest["total_events"] = num_events;
    manifest["avg_particles_per_event"] =
      num_events ? static_cast<double>(total_particles) / static_cast<double>(num_events) : 0.0;
    manifest["avg_event_size_mib"] =
      num_events ? fgs::round3(mean_file_bytes / static_cast<double>(num_events) / kBytesPerMiB) : 0.0;
    manifest["total_particles"] = total_particles;
    manifest["particles_per_event_min"] = min_particles;
    manifest["particles_per_event_max"] = max_particles;

    manifest["write_options"] = write_options_json;
    manifest["write_options_effective"] = {
      {"max_unzipped_page_size_bytes", write_options.GetMaxUnzippedPageSize()}};

    // Raw generated payload per event (both products, uncompressed) -- what the
    // tier's particle count was actually sized to hit, as opposed to
    // avg_event_size_mib above, which is the on-disk, compressed footprint.
    double const bytes_per_particle =
      static_cast<double>(sizeof(products) / sizeof(products[0])) * static_cast<double>(kComponents) *
      sizeof(float);
    manifest["avg_raw_payload_mib"] =
      num_events
        ? fgs::round3(static_cast<double>(total_particles) / static_cast<double>(num_events) *
                       bytes_per_particle / kBytesPerMiB)
        : 0.0;

    manifest["variants"] = json::array();
    for (VariantResult const& v : variants) {
      json vj;
      vj["name"] = v.name;
      vj["dir"] = v.name;
      vj["file"] = (fs::path{v.name} / v.file_name).generic_string();
      vj["file_mib"] = bytes_to_mib(v.bytes);
      if (v.name == "shuffle")
        vj["shuffle_seed"] = shuffle_seed;

      json containers = json::object();
      for (auto const& [name, l] : v.containers) {
        json cj;
        cj["clusters"] = l.clusters;
        cj["pages"] = l.pages;
        containers[name] = cj;
      }
      vj["containers"] = containers;
      manifest["variants"].push_back(vj);
    }

    manifest["products"] = json::array();
    for (auto const& pd : products) {
      json p;
      p["name"] = pd.name;
      p["container"] = pd.container;
      p["container_type"] = "RNTuple";
      p["index_container"] = "index";
      p["index_container_type"] = "TTree";
      manifest["products"].push_back(p);
    }

    std::ofstream{path} << manifest.dump(4) << "\n";
  }

  // Validate the loaded products before writing. Both must describe the same
  // events, each event's flat buffer must be a whole number of particles
  // (kComponents floats each), and the two products must agree on the particle
  // count for every event.
  void validate_products(std::vector<std::vector<float>> const& positions,
                         std::vector<std::vector<float>> const& momenta)
  {
    if (positions.size() != momenta.size())
      throw std::runtime_error("position/momentum event counts differ");

    for (std::uint64_t e = 0; e < positions.size(); ++e) {
      if (positions[e].size() % kComponents != 0)
        throw std::runtime_error("position buffer for event " + std::to_string(e) +
                                 " is not a multiple of kComponents");
      if (momenta[e].size() % kComponents != 0)
        throw std::runtime_error("momentum buffer for event " + std::to_string(e) +
                                 " is not a multiple of kComponents");
      if (positions[e].size() != momenta[e].size())
        throw std::runtime_error("position/momentum particle counts differ at event " +
                                 std::to_string(e));
    }
  }

  // Write one variant's ROOT file: two product RNTuples + the shared index TTree,
  // iterating events in `order`. Each event becomes one row per product (the
  // event's particle vector) plus one index row carrying both products' tokens.
  void write_variant(std::vector<std::vector<float>> const& positions,
                     std::vector<std::vector<float>> const& momenta,
                     std::vector<std::uint64_t> const& order,
                     fs::path const& root_path,
                     ROOT::RNTupleWriteOptions const& wopts)
  {
    auto pos_model = ROOT::RNTupleModel::Create();
    auto fld_pos_ei = pos_model->MakeField<std::uint64_t>("event_id");
    auto fld_vec_pos = pos_model->MakeField<std::vector<fgs::Position>>("vec_particles_pos");

    auto mom_model = ROOT::RNTupleModel::Create();
    auto fld_mom_ei = mom_model->MakeField<std::uint64_t>("event_id");
    auto fld_vec_mom = mom_model->MakeField<std::vector<fgs::Momentum>>("vec_particles_mom");

    auto file = std::unique_ptr<TFile>(TFile::Open(root_path.c_str(), "RECREATE"));
    if (!file || file->IsZombie())
      throw std::runtime_error("failed to open " + root_path.string());

    TTree* index_tree = new TTree("index", "Event index"); // owned by file
    std::uint64_t b_event_id = 0;
    fgs::EventIndex b_index_value;
    index_tree->Branch("event_id", &b_event_id);
    index_tree->Branch("index_value", &b_index_value);

    {
      // Scope ensures writers commit before file->Write().
      auto pos_writer =
        ROOT::RNTupleWriter::Append(std::move(pos_model), kPositionContainer, *file, wopts);
      auto mom_writer =
        ROOT::RNTupleWriter::Append(std::move(mom_model), kMomentumContainer, *file, wopts);

      for (std::uint64_t event_id : order) {
        std::vector<float> const& pf = positions[event_id];
        std::uint64_t const pos_n = pf.size() / kComponents;
        fld_vec_pos->clear();
        fld_vec_pos->reserve(pos_n);
        for (std::uint64_t i = 0; i < pos_n; ++i)
          fld_vec_pos->push_back({pf[kComponents * i], pf[kComponents * i + 1], pf[kComponents * i + 2]});
        *fld_pos_ei = event_id;

        std::vector<float> const& mf = momenta[event_id];
        std::uint64_t const mom_n = mf.size() / kComponents;
        fld_vec_mom->clear();
        fld_vec_mom->reserve(mom_n);
        for (std::uint64_t i = 0; i < mom_n; ++i)
          fld_vec_mom->push_back({mf[kComponents * i], mf[kComponents * i + 1], mf[kComponents * i + 2]});
        *fld_mom_ei = event_id;

        // GetNEntries() is the row each product's next Fill() lands at.
        b_event_id = event_id;
        b_index_value.clear();
        b_index_value["position"] = {kPositionContainer, pos_writer->GetNEntries()};
        b_index_value["momentum"] = {kMomentumContainer, mom_writer->GetNEntries()};

        pos_writer->Fill();
        mom_writer->Fill();
        index_tree->Fill();
      }
    }

    index_tree->BuildIndex("event_id");
    file->Write();
    file->Close();
  }
}

int main(int argc, char** argv)
{
  fs::path config_path =
    (argc > 1) ? fs::path{argv[1]} : fs::path{"configs/writing/strategy_one.json"};

  try {
    StrategyConfig cfg = load_strategy_config(config_path);

    // Eager-load each product independently, once, outside any timer.
    std::vector<std::vector<float>> positions = fgs::load_product(cfg.gen_dir, "positions.bin");
    std::vector<std::vector<float>> momenta = fgs::load_product(cfg.gen_dir, "momenta.bin");
    validate_products(positions, momenta);

    std::uint64_t num_events = positions.size();
    std::uint64_t total_particles = 0;
    std::uint64_t min_particles = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_particles = 0;
    for (auto const& pf : positions) {
      std::uint64_t const n = pf.size() / kComponents;
      total_particles += n;
      min_particles = std::min(min_particles, n);
      max_particles = std::max(max_particles, n);
    }
    if (positions.empty())
      min_particles = 0;
    std::cout << "loaded " << num_events << " events, " << total_particles << " particles\n";
    std::cout << "max unzipped page size: " << cfg.write_options.GetMaxUnzippedPageSize()
              << " bytes"
              << (cfg.write_options_json.contains("max_page_size_mib") ? "" : " (ROOT default)")
              << "\n";

    std::vector<VariantResult> results;
    for (std::string const& variant : cfg.variants) {
      std::vector<std::uint64_t> order = fgs::write_order(num_events, variant, cfg.shuffle_seed);

      fs::path dir = cfg.output_root / variant;
      fs::create_directories(dir);
      std::string file_name = root_file_for(variant);
      fs::path root_path = dir / file_name;

      auto t_start = std::chrono::steady_clock::now();
      write_variant(positions, momenta, order, root_path, cfg.write_options);
      auto t_end = std::chrono::steady_clock::now();

      auto bytes = static_cast<std::uint64_t>(fs::file_size(root_path));

      double wall_s = std::chrono::duration<double>(t_end - t_start).count();
      std::cout << "variant   : " << variant << "\n"
                << "  output  : " << root_path << "\n"
                << "  size    : " << bytes << " bytes\n"
                << "  wall    : " << wall_s << " s\n";

      VariantResult vr{variant, file_name, bytes, {}};
      for (std::string const& container : {kPositionContainer, kMomentumContainer}) {
        Layout l = inspect_layout(container, root_path);
        vr.containers.emplace_back(container, l);
        std::cout << "  " << container << ": clusters=" << l.clusters << " pages=" << l.pages
                  << "\n";
      }
      results.push_back(std::move(vr));
    }

    // One manifest for the whole strategy (product registry + variant list).
    fs::create_directories(cfg.output_root);
    write_manifest(cfg.output_root / "manifest.json", num_events, total_particles, min_particles,
                  max_particles, results, cfg.shuffle_seed, cfg.write_options_json,
                  cfg.write_options);

    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fgs_strategy_one: error: " << e.what() << "\n";
    return 1;
  }
}
