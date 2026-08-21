#include "fgs/event_reader.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include <ROOT/REntry.hxx>
#include <ROOT/RNTupleDescriptor.hxx>
#include <ROOT/RNTupleModel.hxx>
#include <TFile.h>
#include <TTree.h>

#include "fgs/token.hpp"

namespace fgs {

  namespace {

    constexpr std::uint64_t kNoEntry = std::numeric_limits<std::uint64_t>::max();

    // Storage width of everything below `field`, which for a collection field is
    // its elements and excludes the offset column the field itself owns.
    std::uint64_t payload_bits(ROOT::RNTupleDescriptor const& desc,
                               ROOT::RFieldDescriptor const& field)
    {
      std::uint64_t bits = 0;
      for (auto const& sub : desc.GetFieldIterable(field)) {
        for (auto const& col : desc.GetColumnIterable(sub)) {
          // Alternate representations of one column describe the same values.
          if (col.GetRepresentationIndex() == 0)
            bits += col.GetBitsOnStorage();
        }
        bits += payload_bits(desc, sub);
      }
      return bits;
    }

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

    // One sequential scan collects the row numbers and the container each product
    // lives in; the TTree is never touched again. The product set is only known
    // once the scan ends, so rows are gathered per product and flattened below.
    std::uint64_t ev = 0;
    EventIndex* iv = nullptr;
    index->SetBranchAddress("event_id", &ev);
    index->SetBranchAddress("index_value", &iv);

    std::map<std::string, std::string> container_of;
    std::map<std::string, std::size_t> column_of;
    std::vector<std::vector<std::uint64_t>> columns;
    std::vector<bool> seen;
    std::uint64_t distinct = 0;

    Long64_t const n = index->GetEntries();
    for (Long64_t i = 0; i < n; ++i) {
      index->GetEntry(i);
      if (!iv)
        continue;
      if (seen.size() <= ev)
        seen.resize(ev + 1, false);
      if (!seen[ev]) {
        seen[ev] = true;
        ++distinct;
      }
      for (auto const& [product, token] : *iv) {
        container_of.emplace(product, token.container);
        auto const [cit, added] = column_of.emplace(product, columns.size());
        if (added)
          columns.emplace_back();
        std::vector<std::uint64_t>& column = columns[cit->second];
        if (column.size() <= ev)
          column.resize(ev + 1, kNoEntry);
        column[ev] = token.entry;
      }
    }
    index->ResetBranchAddresses();

    // Count distinct events, not index entries, and demand a dense id range so a
    // mismatch fails here rather than as "event not found" mid-benchmark.
    num_events_ = distinct;
    for (std::uint64_t id = 0; id < num_events_; ++id)
      if (id >= seen.size() || !seen[id])
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

    std::size_t const n_products = product_names_.size();
    entries_.assign(num_events_ * n_products, kNoEntry);
    for (std::size_t p = 0; p < n_products; ++p) {
      std::vector<std::uint64_t> const& column = columns[column_of.at(product_names_[p])];
      std::uint64_t const rows = std::min<std::uint64_t>(num_events_, column.size());
      for (std::uint64_t id = 0; id < rows; ++id)
        entries_[id * n_products + p] = column[id];
    }
    columns.clear();
    columns.shrink_to_fit();

    // Open every container up front so read_product pays no open cost inside the
    // timed loop, and bind one per product so the read path never looks up a name.
    for (std::string const& product : product_names_) {
      Container& c = container_for(container_of.at(product));
      product_containers_.push_back(&c);
      product_element_bytes_.push_back(c.element_bytes);
    }
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
      c.element_bytes = payload_bits(c.reader->GetDescriptor(), field) / 8;
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

  EventReader::Location EventReader::locate(std::uint64_t event_id,
                                            std::string const& product) const
  {
    if (event_id >= num_events_)
      throw std::runtime_error("EventReader: event " + std::to_string(event_id) +
                               " not found in index");

    auto const missing = [&] {
      return std::runtime_error("EventReader: product \"" + product + "\" not found for event " +
                                std::to_string(event_id));
    };

    auto const it = std::lower_bound(product_names_.begin(), product_names_.end(), product);
    if (it == product_names_.end() || *it != product)
      throw missing();

    std::size_t const p = static_cast<std::size_t>(it - product_names_.begin());
    std::uint64_t const entry = entries_[event_id * product_names_.size() + p];
    if (entry == kNoEntry)
      throw missing();
    return {entry, p};
  }

  std::size_t EventReader::read_product(std::uint64_t event_id, std::string const& product)
  {
    Location const loc = locate(event_id, product);
    Container& c = *product_containers_[loc.product];
    c.reader->LoadEntry(loc.entry); // whole-row read: refreshes the bound vector
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
