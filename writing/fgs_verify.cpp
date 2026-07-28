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
#include <ROOT/RNTupleView.hxx>
#include <TFile.h>
#include <TTree.h>
#include <cstdint>
#include <cstdlib>
#include "fgs/bin_io.hpp"
#include "fgs/token.hpp"
#include "fgs/types.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
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

  // The container name for a product, read from the manifest (not hard-coded).
  std::string container_of(nlohmann::json const& manifest, std::string const& name)
  {
    for (auto const& p : manifest.at("products"))
      if (p.at("name").get<std::string>() == name)
        return p.at("container").get<std::string>();
    require(false, "manifest missing product \"" + name + "\"");
    return {};
  }

  // Read the whole index TTree into an event_id -> tokens map, once.
  std::unordered_map<std::uint64_t, fgs::EventIndex> load_index(TTree* index)
  {
    std::uint64_t ev = 0;
    fgs::EventIndex* iv = nullptr;
    index->SetBranchAddress("event_id", &ev);
    index->SetBranchAddress("index_value", &iv);

    std::unordered_map<std::uint64_t, fgs::EventIndex> out;
    Long64_t const n = index->GetEntries();
    for (Long64_t i = 0; i < n; ++i) {
      index->GetEntry(i);
      if (iv)
        out[ev] = *iv;
    }
    index->ResetBranchAddresses();
    return out;
  }

  // Verify one variant's file against the reference events: for each target event
  // the index token locates its row in each product container, and the stored
  // particle vector must match the reference exactly.
  void verify_variant(fs::path const& root_path,
                      std::string const& variant,
                      std::string const& pos_container,
                      std::string const& mom_container,
                      std::vector<fgs::Event> const& events,
                      std::vector<std::uint64_t> const& user_events,
                      bool check_all)
  {
    std::uint64_t const num_events = events.size();

    std::cout << "=== variant " << variant << " (" << root_path.string() << ") ===\n";

    auto file = std::unique_ptr<TFile>(TFile::Open(root_path.c_str(), "READ"));
    require(file && !file->IsZombie(), "cannot open " + root_path.string());
    TTree* index = dynamic_cast<TTree*>(file->Get("index"));
    require(index, "index TTree not found in " + root_path.string());

    std::unordered_map<std::uint64_t, fgs::EventIndex> tokens = load_index(index);

    auto pos_reader = ROOT::RNTupleReader::Open(pos_container, root_path.string());
    auto pos_vec = pos_reader->GetView<std::vector<fgs::Position>>("vec_particles_pos");
    auto pos_eid = pos_reader->GetView<std::uint64_t>("event_id");

    auto mom_reader = ROOT::RNTupleReader::Open(mom_container, root_path.string());
    auto mom_vec = mom_reader->GetView<std::vector<fgs::Momentum>>("vec_particles_mom");
    auto mom_eid = mom_reader->GetView<std::uint64_t>("event_id");

    std::vector<std::uint64_t> targets;
    if (check_all) {
      targets.resize(num_events);
      std::iota(targets.begin(), targets.end(), std::uint64_t{0});
    } else {
      targets = user_events;
    }

    for (std::uint64_t target : targets) {
      fgs::Event const& expected = events[target];

      auto tit = tokens.find(target);
      require(tit != tokens.end(), variant + ": event " + std::to_string(target) + " not in index");
      fgs::EventIndex const& idx = tit->second;

      auto pt = idx.find("position");
      require(pt != idx.end(), variant + ": event " + std::to_string(target) + " has no position token");
      require(pt->second.container == pos_container, variant + ": position container mismatch");
      auto const& pv = pos_vec(pt->second.entry);
      require(pos_eid(pt->second.entry) == target, variant + ": position row event_id mismatch");
      require(pv.size() == expected.positions.size(), variant + ": position count mismatch");
      for (std::size_t i = 0; i < pv.size(); ++i)
        require(pv[i].x == expected.positions[i].x && pv[i].y == expected.positions[i].y &&
                  pv[i].z == expected.positions[i].z,
                variant + ": position value mismatch at event " + std::to_string(target));

      auto mt = idx.find("momentum");
      require(mt != idx.end(), variant + ": event " + std::to_string(target) + " has no momentum token");
      require(mt->second.container == mom_container, variant + ": momentum container mismatch");
      auto const& mv = mom_vec(mt->second.entry);
      require(mom_eid(mt->second.entry) == target, variant + ": momentum row event_id mismatch");
      require(mv.size() == expected.momenta.size(), variant + ": momentum count mismatch");
      for (std::size_t i = 0; i < mv.size(); ++i)
        require(mv[i].px == expected.momenta[i].px && mv[i].py == expected.momenta[i].py &&
                  mv[i].pz == expected.momenta[i].pz,
                variant + ": momentum value mismatch at event " + std::to_string(target));

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

  nlohmann::json manifest;
  {
    std::ifstream in(strat_root / "manifest.json");
    require(!!in, "cannot open " + (strat_root / "manifest.json").string());
    in >> manifest;
  }

  std::uint64_t num_events = manifest.at("total_events").get<std::uint64_t>();
  std::string pos_container = container_of(manifest, "position");
  std::string mom_container = container_of(manifest, "momentum");

  // Guard against a gen dir / root file mismatch: events[target] below indexes
  // the Phase 1 events, so the counts must agree or we would read out of bounds.
  require(events.size() == num_events,
          "gen dir has " + std::to_string(events.size()) + " events but manifest says " +
            std::to_string(num_events) + " (regenerate or re-run the writer)");

  for (std::uint64_t id : user_events)
    require(id < num_events, "requested event " + std::to_string(id) + " is out of range (" +
                               std::to_string(num_events) + " events)");

  auto const& variants = manifest.at("variants");
  for (auto const& v : variants) {
    std::string variant = v.at("name").get<std::string>();
    fs::path root_path = strat_root / v.at("file").get<std::string>();
    verify_variant(
      root_path, variant, pos_container, mom_container, events, user_events, check_all);
  }

  std::cout << "\nall " << variants.size() << " variant(s) verified -- index returns identical "
            << "data for ordered and shuffled layouts\n";
  return 0;
}
