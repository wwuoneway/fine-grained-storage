// fgs_verify: cross-check strategy_one output against the Phase 1 binaries, for
// every write variant (no-shuffle, shuffle).
//
// Usage: fgs_verify [--all] [--events a,b,c] [strategy_output_root [gen_dir]]
//   --all                 verify every event
//   --events a,b,c        verify these specific event ids
//   (one of --all or --events is required)
//   strategy_output_root  default: output/writing/rntuple/strategy_one
//   gen_dir               default: output/generation

#include <ROOT/RNTupleReader.hxx>
#include <TFile.h>
#include <TTree.h>
#include <cstdint>
#include <cstdlib>
#include <fgs/bin_io.hpp>
#include <fgs/types.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
  void require(bool cond, std::string const& msg)
  {
    if (!cond) {
      std::cerr << "FAIL: " << msg << "\n";
      std::abort();
    }
  }

  // The numeric minor key for a product, read from the manifest (not hard-coded).
  std::uint64_t product_id(nlohmann::json const& manifest, std::string const& name)
  {
    for (auto const& p : manifest.at("products"))
      if (p.at("name").get<std::string>() == name)
        return p.at("product_id").get<std::uint64_t>();
    require(false, "manifest missing product \"" + name + "\"");
    return 0;
  }

  // Verify one variant's strategy_one.root against the reference events. The
  // product registry (pos_id/mom_id) comes from the shared strategy manifest.
  void verify_variant(fs::path const& root_path,
                      std::string const& variant,
                      std::uint64_t pos_id,
                      std::uint64_t mom_id,
                      std::vector<fgs::Event> const& events,
                      std::vector<std::uint64_t> const& user_events,
                      bool check_all)
  {
    std::uint64_t num_events = events.size();

    std::cout << "=== variant " << variant << " (" << root_path.string() << ") ===\n";

    auto file = std::unique_ptr<TFile>(TFile::Open(root_path.c_str(), "READ"));
    require(file && !file->IsZombie(), "cannot open " + root_path.string());
    TTree* index = dynamic_cast<TTree*>(file->Get("index"));
    require(index, "index TTree not found in " + root_path.string());

    std::uint64_t b_row_start, b_row_count;
    index->SetBranchAddress("row_start", &b_row_start);
    index->SetBranchAddress("row_count", &b_row_count);

    auto pos_reader = ROOT::RNTupleReader::Open("position", root_path.string());
    auto px = pos_reader->GetView<float>("x");
    auto py = pos_reader->GetView<float>("y");
    auto pz = pos_reader->GetView<float>("z");
    auto pev = pos_reader->GetView<std::uint64_t>("event_id");

    auto mom_reader = ROOT::RNTupleReader::Open("momentum", root_path.string());
    auto mx = mom_reader->GetView<float>("px");
    auto my = mom_reader->GetView<float>("py");
    auto mz = mom_reader->GetView<float>("pz");
    auto mev = mom_reader->GetView<std::uint64_t>("event_id");

    // --all verifies every event (exhaustive); otherwise verify exactly the
    // event ids the user requested via --events (validated in main()).
    std::vector<std::uint64_t> targets;
    if (check_all) {
      targets.resize(num_events);
      std::iota(targets.begin(), targets.end(), std::uint64_t{0});
    } else {
      targets = user_events;
    }

    for (std::uint64_t target : targets) {
      fgs::Event const& expected = events[target];

      // --- position ---
      Long64_t e = index->GetEntryNumberWithIndex(static_cast<Long64_t>(target),
                                                  static_cast<Long64_t>(pos_id));
      require(e >= 0, variant + ": event " + std::to_string(target) + " not in index (position)");
      index->GetEntry(e);
      require(b_row_count == expected.positions.size(), variant + ": position count mismatch");
      for (std::uint64_t i = 0; i < b_row_count; ++i) {
        std::uint64_t r = b_row_start + i;
        require(pev(r) == target, variant + ": position row event_id mismatch");
        require(px(r) == expected.positions[i].x && py(r) == expected.positions[i].y &&
                  pz(r) == expected.positions[i].z,
                variant + ": position value mismatch at event " + std::to_string(target));
      }

      // --- momentum ---
      e = index->GetEntryNumberWithIndex(static_cast<Long64_t>(target),
                                         static_cast<Long64_t>(mom_id));
      require(e >= 0, variant + ": event " + std::to_string(target) + " not in index (momentum)");
      index->GetEntry(e);
      require(b_row_count == expected.momenta.size(), variant + ": momentum count mismatch");
      for (std::uint64_t i = 0; i < b_row_count; ++i) {
        std::uint64_t r = b_row_start + i;
        require(mev(r) == target, variant + ": momentum row event_id mismatch");
        require(mx(r) == expected.momenta[i].px && my(r) == expected.momenta[i].py &&
                  mz(r) == expected.momenta[i].pz,
                variant + ": momentum value mismatch at event " + std::to_string(target));
      }

      if (!check_all)
        std::cout << "  event " << target << " PASSED (" << expected.positions.size()
                  << " particles, position + momentum)\n";
    }

    if (check_all)
      std::cout << "  all " << targets.size() << " events PASSED (position + momentum)\n";
  }
}

int main(int argc, char** argv)
{
  // Parse args: --all and/or --events a,b,c, plus up to two positional paths.
  bool check_all = false;
  std::vector<std::uint64_t> user_events;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--all") {
      check_all = true;
    } else if (a == "--events") {
      require(i + 1 < argc, "--events requires a comma-separated list of event ids");
      std::stringstream ss(argv[++i]);
      std::string tok;
      while (std::getline(ss, tok, ','))
        if (!tok.empty())
          user_events.push_back(std::stoull(tok));
    } else {
      positional.push_back(a);
    }
  }
  require(check_all || !user_events.empty(), "specify --all or --events a,b,c");
  fs::path strat_root = positional.size() > 0 ? fs::path{positional[0]}
                                              : fs::path{"output/writing/rntuple/strategy_one"};
  fs::path gen_dir =
    positional.size() > 1 ? fs::path{positional[1]} : fs::path{"output/generation"};

  // Reference data for cross-check (outside any timing concern).
  std::vector<fgs::Event> events = fgs::load_all_events(gen_dir);

  // The single strategy manifest lists the product registry and every variant.
  nlohmann::json manifest;
  {
    std::ifstream in(strat_root / "manifest.json");
    require(!!in, "cannot open " + (strat_root / "manifest.json").string());
    in >> manifest;
  }

  std::uint64_t num_events = manifest.at("num_events").get<std::uint64_t>();
  std::uint64_t pos_id = product_id(manifest, "position");
  std::uint64_t mom_id = product_id(manifest, "momentum");

  // Guard against a gen dir / root file mismatch: events[target] below indexes
  // the Phase 1 events, so the counts must agree or we would read out of bounds.
  require(events.size() == num_events,
          "gen dir has " + std::to_string(events.size()) + " events but manifest says " +
            std::to_string(num_events) + " (regenerate or re-run the writer)");

  // Reject any requested event id that does not exist before reading.
  for (std::uint64_t id : user_events)
    require(id < num_events, "requested event " + std::to_string(id) + " is out of range (" +
                               std::to_string(num_events) + " events)");

  auto const& variants = manifest.at("variants");
  for (auto const& v : variants) {
    std::string variant = v.at("name").get<std::string>();
    // The manifest gives each variant's data file relative to the strategy root.
    fs::path root_path = strat_root / v.at("file").get<std::string>();
    verify_variant(root_path, variant, pos_id, mom_id, events, user_events, check_all);
  }

  std::cout << "\nall " << variants.size() << " variant(s) verified — index returns identical "
            << "data for ordered and shuffled layouts\n";
  return 0;
}
