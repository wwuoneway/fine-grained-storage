#include "fgs/event_reader.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

#include <ROOT/REntry.hxx>
#include <ROOT/RNTupleDescriptor.hxx>
#include <ROOT/RNTupleModel.hxx>
#include <TFile.h>
#include <TTree.h>

namespace fgs {

  namespace {

    std::string join(std::vector<std::string> const& names)
    {
      std::string out;
      for (std::string const& name : names) {
        if (!out.empty())
          out += ", ";
        out += name;
      }
      return out;
    }

  }

  EventReader::EventReader(std::filesystem::path const& root_file,
                           std::string const& index_name,
                           ROOT::RNTupleReadOptions opts,
                           std::vector<std::string> const& products)
    : root_file_(root_file), opts_(std::move(opts))
  {
    auto file = std::unique_ptr<TFile>(TFile::Open(root_file_.c_str(), "READ"));
    if (!file || file->IsZombie())
      throw std::runtime_error("EventReader: cannot open " + root_file_.string());

    TTree* index = dynamic_cast<TTree*>(file->Get(index_name.c_str()));
    if (!index)
      throw std::runtime_error("EventReader: index TTree \"" + index_name + "\" not found in " +
                               root_file_.string());

    // One sequential scan builds the event -> tokens map and learns which
    // container each product lives in. After this the TTree is never accessed
    // again.
    std::uint64_t ev = 0;
    EventIndex* iv = nullptr;
    index->SetBranchAddress("event_id", &ev);
    index->SetBranchAddress("index_value", &iv);

    std::map<std::string, std::string> container_of;
    Long64_t const n = index->GetEntries();
    for (Long64_t i = 0; i < n; ++i) {
      index->GetEntry(i);
      if (!iv)
        continue;
      index_[ev] = *iv;
      for (auto const& [product, token] : *iv)
        container_of.emplace(product, token.container);
    }
    index->ResetBranchAddresses();

    // Count distinct events, not index entries, and demand a dense id range so a
    // mismatch fails here rather than as "event not found" mid-benchmark.
    num_events_ = static_cast<std::uint64_t>(index_.size());
    for (std::uint64_t id = 0; id < num_events_; ++id)
      if (index_.find(id) == index_.end())
        throw std::runtime_error("EventReader: index in " + root_file_.string() + " has " +
                                 std::to_string(num_events_) + " distinct events but event id " +
                                 std::to_string(id) + " is missing (ids must cover [0, N))");

    for (auto const& [product, container] : container_of)
      product_names_.push_back(product);

    if (!products.empty()) {
      for (std::string const& product : products)
        if (container_of.find(product) == container_of.end())
          throw std::runtime_error("EventReader: product \"" + product + "\" requested but not in " +
                                   root_file_.string() +
                                   " (available: " + join(product_names_) + ")");
      std::set<std::string> const selected(products.begin(), products.end());
      product_names_.assign(selected.begin(), selected.end());
    }

    std::set<std::string> containers;
    for (std::string const& product : product_names_)
      containers.insert(container_of.at(product));

    // Open every container up front so read_product pays no open cost inside the
    // timed loop.
    for (std::string const& name : containers)
      container_for(name);
  }

  EventReader::~EventReader() = default;

  EventReader::Container& EventReader::container_for(std::string const& name)
  {
    auto it = containers_.find(name);
    if (it != containers_.end())
      return it->second;

    Container c;
    c.reader = ROOT::RNTupleReader::Open(name, root_file_.string(), opts_);

    // The container has one data field besides event_id; bind the typed pointer
    // matching its element type. LoadEntry() later refreshes it per row.
    ROOT::REntry const& entry = c.reader->GetModel().GetDefaultEntry();
    std::string field_name;
    std::string type_name;
    for (auto const& field : c.reader->GetDescriptor().GetTopLevelFields()) {
      if (field.GetFieldName() == "event_id")
        continue;
      field_name = field.GetFieldName();
      type_name = field.GetTypeName();
      break;
    }

    if (type_name.find("fgs::Position") != std::string::npos)
      c.pos = entry.GetPtr<std::vector<Position>>(field_name);
    else if (type_name.find("fgs::Momentum") != std::string::npos)
      c.mom = entry.GetPtr<std::vector<Momentum>>(field_name);
    else
      throw std::runtime_error("EventReader: container \"" + name +
                               "\" has unsupported field type \"" + type_name + "\"");

    return containers_.emplace(name, std::move(c)).first->second;
  }

  Token const& EventReader::locate(std::uint64_t event_id, std::string const& product)
  {
    auto eit = index_.find(event_id);
    if (eit == index_.end())
      throw std::runtime_error("EventReader: event " + std::to_string(event_id) +
                               " not found in index");
    auto pit = eit->second.find(product);
    if (pit == eit->second.end())
      throw std::runtime_error("EventReader: product \"" + product + "\" not found for event " +
                               std::to_string(event_id));
    return pit->second;
  }

  std::size_t EventReader::read_product(std::uint64_t event_id, std::string const& product)
  {
    using clock = std::chrono::steady_clock;
    auto const ns = [](auto d) { return std::chrono::duration<double, std::nano>(d).count(); };

    clock::time_point t0;
    if (instrument_) t0 = clock::now();
    Token const& token = locate(event_id, product);
    Container& c = container_for(token.container);
    if (instrument_) timers_.locate_ns += ns(clock::now() - t0);

    clock::time_point tl;
    if (instrument_) tl = clock::now();
    c.reader->LoadEntry(token.entry); // whole-row read: refreshes the bound vector
    if (instrument_) timers_.load_ns += ns(clock::now() - tl);

    return c.pos ? c.pos->size() : c.mom->size();
  }

  std::map<std::string, EventReader::ContainerFacts> EventReader::dataset_facts() const
  {
    std::map<std::string, ContainerFacts> out;
    for (auto const& [name, c] : containers_) {
      ContainerFacts facts;
      auto const& desc = c.reader->GetDescriptor();
      facts.clusters = desc.GetNClusters();
      for (auto const& cluster : desc.GetClusterIterable()) {
        for (auto const& col : desc.GetColumnIterable()) {
          ROOT::DescriptorId_t const colId = col.GetPhysicalId();
          if (!cluster.ContainsColumn(colId))
            continue;
          facts.pages += cluster.GetPageRange(colId).GetPageInfos().size();
        }
      }
      out.emplace(name, facts);
    }
    return out;
  }

  void EventReader::print_metrics() const { print_metrics(std::cout); }

  void EventReader::print_metrics(std::ostream& os) const
  {
    for (auto const& [name, c] : containers_) {
      os << "=== metrics: " << name << " ===\n";
      c.reader->PrintInfo(ROOT::ENTupleInfo::kMetrics, os);
    }
  }

}
