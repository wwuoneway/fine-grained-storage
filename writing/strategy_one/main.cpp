// fgs_strategy_one: write strategy "strategy_one" — two data-product RNTuples
// (position, momentum) plus a shared TTree index, both inside one ROOT file.
//
// It emits one output folder per write variant under the strategy's output root:
//   <output_root>/no-shuffle/strategy_one.root   (events written in event order)
//   <output_root>/shuffle/strategy_one.root     (events written in a seeded shuffle)
// Each folder also carries its own manifest.json. The shuffle changes only the
// physical row layout — the index makes reads order-independent either way.
//
// Usage: fgs_strategy_one [config.json]   (default: configs/writing/strategy_one.json)

#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleWriter.hxx>
#include <TFile.h>
#include <TTree.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "common/ordering.hpp"
#include "fgs/bin_io.hpp"

namespace fs = std::filesystem;

namespace {
  // product_id encoding: the TTree index needs a numeric minor key. Persisted in
  // each manifest so readers don't hard-code it. 0 = position, 1 = momentum.
  constexpr std::uint64_t PRODUCT_ID_POSITION = 0;
  constexpr std::uint64_t PRODUCT_ID_MOMENTUM = 1;

  // Components per particle in a product's flat float buffer (x,y,z / px,py,pz).
  constexpr std::uint64_t kComponents = 3;

  struct StrategyConfig {
    fs::path gen_dir;
    fs::path output_root;
    std::vector<std::string> variants;
    std::uint64_t shuffle_seed = 0;
  };

  StrategyConfig load_strategy_config(fs::path const& path)
  {
    std::ifstream in(path);
    if (!in)
      throw std::runtime_error("cannot open config: " + path.string());

    nlohmann::json j;
    in >> j;

    StrategyConfig cfg;
    cfg.gen_dir = j.at("gen_dir").get<std::string>();
    cfg.output_root = j.at("output_root").get<std::string>();
    cfg.variants = j.at("variants").get<std::vector<std::string>>();
    cfg.shuffle_seed = j.at("shuffle_seed").get<std::uint64_t>();
    return cfg;
  }

  struct VariantResult {
    std::string name;
    std::string file_name;
    std::uint64_t bytes;
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
                      std::vector<VariantResult> const& variants,
                      std::uint64_t shuffle_seed)
  {
    struct ProductDesc {
      char const* name;
      std::uint64_t id;
    };
    constexpr ProductDesc products[] = {{"position", PRODUCT_ID_POSITION},
                                        {"momentum", PRODUCT_ID_MOMENTUM}};

    nlohmann::json manifest;
    manifest["strategy"] = "strategy_one";
    manifest["num_events"] = num_events;
    manifest["total_particles"] = total_particles;

    manifest["variants"] = nlohmann::json::array();
    for (VariantResult const& v : variants) {
      nlohmann::json vj;
      vj["name"] = v.name;
      vj["dir"] = v.name;
      vj["file"] = (fs::path{v.name} / v.file_name).generic_string();
      vj["file_bytes"] = v.bytes;
      if (v.name == "shuffle")
        vj["shuffle_seed"] = shuffle_seed;
      manifest["variants"].push_back(vj);
    }

    manifest["products"] = nlohmann::json::array();
    for (auto const& pd : products) {
      nlohmann::json p;
      p["name"] = pd.name;
      p["product_id"] = pd.id;
      p["data_container"] = pd.name;
      p["data_container_type"] = "RNTuple";
      p["index_container"] = "index";
      p["index_container_type"] = "TTree";
      manifest["products"].push_back(p);
    }

    std::ofstream{path} << manifest.dump(4) << "\n";
  }

  // Validate the loaded products before writing. Both must describe the same
  // events, each event's flat buffer must be a whole number of particles
  // (kComponents floats each), and the two products must agree on the particle
  // count for every event — otherwise their index row ranges would not line up.
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
  // iterating events in `order`. Each product's flat float buffer is kComponents per particle.
  void write_variant(std::vector<std::vector<float>> const& positions,
                     std::vector<std::vector<float>> const& momenta,
                     std::vector<std::uint64_t> const& order,
                     fs::path const& root_path)
  {
    auto pos_model = ROOT::RNTupleModel::Create();
    auto fld_pos_ei = pos_model->MakeField<std::uint64_t>("event_id");
    auto fld_x = pos_model->MakeField<float>("x");
    auto fld_y = pos_model->MakeField<float>("y");
    auto fld_z = pos_model->MakeField<float>("z");

    auto mom_model = ROOT::RNTupleModel::Create();
    auto fld_mom_ei = mom_model->MakeField<std::uint64_t>("event_id");
    auto fld_px = mom_model->MakeField<float>("px");
    auto fld_py = mom_model->MakeField<float>("py");
    auto fld_pz = mom_model->MakeField<float>("pz");

    auto file = std::unique_ptr<TFile>(TFile::Open(root_path.c_str(), "RECREATE"));
    if (!file || file->IsZombie())
      throw std::runtime_error("failed to open " + root_path.string());

    // One row per (event, product). BuildIndex(event_id, product_id) -> O(log N) lookup.
    TTree* index_tree = new TTree("index", "Event index"); // owned by file
    std::uint64_t b_event_id, b_product_id, b_row_start, b_row_count;
    std::string b_product_name, b_container_name;
    index_tree->Branch("event_id", &b_event_id);
    index_tree->Branch("product_id", &b_product_id);
    index_tree->Branch("product_name", &b_product_name);
    index_tree->Branch("container_name", &b_container_name);
    index_tree->Branch("row_start", &b_row_start);
    index_tree->Branch("row_count", &b_row_count);

    {
      // Scope ensures writers commit before file->Write().
      auto pos_writer = ROOT::RNTupleWriter::Append(std::move(pos_model), "position", *file);
      auto mom_writer = ROOT::RNTupleWriter::Append(std::move(mom_model), "momentum", *file);

      for (std::uint64_t event_id : order) {
        std::vector<float> const& pf = positions[event_id];
        std::uint64_t pos_n = pf.size() / kComponents;
        std::uint64_t pos_start = pos_writer->GetNEntries();
        for (std::uint64_t i = 0; i < pos_n; ++i) {
          *fld_pos_ei = event_id;
          *fld_x = pf[kComponents * i];
          *fld_y = pf[kComponents * i + 1];
          *fld_z = pf[kComponents * i + 2];
          pos_writer->Fill();
        }
        b_event_id = event_id;
        b_product_id = PRODUCT_ID_POSITION;
        b_product_name = "position";
        b_container_name = "position";
        b_row_start = pos_start;
        b_row_count = pos_n;
        index_tree->Fill();

        std::vector<float> const& mf = momenta[event_id];
        std::uint64_t mom_n = mf.size() / kComponents;
        std::uint64_t mom_start = mom_writer->GetNEntries();
        for (std::uint64_t i = 0; i < mom_n; ++i) {
          *fld_mom_ei = event_id;
          *fld_px = mf[kComponents * i];
          *fld_py = mf[kComponents * i + 1];
          *fld_pz = mf[kComponents * i + 2];
          mom_writer->Fill();
        }
        b_event_id = event_id;
        b_product_id = PRODUCT_ID_MOMENTUM;
        b_product_name = "momentum";
        b_container_name = "momentum";
        b_row_start = mom_start;
        b_row_count = mom_n;
        index_tree->Fill();
      }
    }

    index_tree->BuildIndex("event_id", "product_id");
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
    for (auto const& pf : positions)
      total_particles += pf.size() / kComponents;
    std::cout << "loaded " << num_events << " events, " << total_particles << " particles\n";

    std::vector<VariantResult> results;
    for (std::string const& variant : cfg.variants) {
      std::vector<std::uint64_t> order = fgs::write_order(num_events, variant, cfg.shuffle_seed);

      fs::path dir = cfg.output_root / variant;
      fs::create_directories(dir);
      std::string file_name = root_file_for(variant);
      fs::path root_path = dir / file_name;

      auto t_start = std::chrono::steady_clock::now();
      write_variant(positions, momenta, order, root_path);
      auto t_end = std::chrono::steady_clock::now();

      auto bytes = static_cast<std::uint64_t>(fs::file_size(root_path));
      results.push_back({variant, file_name, bytes});

      double wall_s = std::chrono::duration<double>(t_end - t_start).count();
      std::cout << "variant   : " << variant << "\n"
                << "  output  : " << root_path << "\n"
                << "  size    : " << bytes << " bytes\n"
                << "  wall    : " << wall_s << " s\n";
    }

    // One manifest for the whole strategy (product registry + variant list).
    fs::create_directories(cfg.output_root);
    write_manifest(cfg.output_root / "manifest.json",
                   num_events,
                   total_particles,
                   results,
                   cfg.shuffle_seed);

    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fgs_strategy_one: error: " << e.what() << "\n";
    return 1;
  }
}
