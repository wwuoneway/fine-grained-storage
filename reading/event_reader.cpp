#include "event_reader.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <ROOT/REntry.hxx>
#include <ROOT/RNTupleDescriptor.hxx>
#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleView.hxx>
#include <TFile.h>
#include <TTree.h>

namespace fgs {

  EventReader::EventReader(std::filesystem::path const& root_file,
                           std::string const& index_name,
                           std::vector<ProductSpec> const& products,
                           ROOT::RNTupleReadOptions opts)
  {
    file_.reset(TFile::Open(root_file.c_str(), "READ"));
    if (!file_ || file_->IsZombie())
      throw std::runtime_error("EventReader: cannot open " + root_file.string());

    index_ = dynamic_cast<TTree*>(file_->Get(index_name.c_str()));
    if (!index_)
      throw std::runtime_error("EventReader: index TTree \"" + index_name + "\" not found in " +
                               root_file.string());

    for (ProductSpec const& spec : products) {
      Product prod;
      prod.id = spec.id;
      prod.reader = ROOT::RNTupleReader::Open(spec.name, root_file.string(), opts);

      // Discover the component columns from the RNTuple itself (every top-level
      // field except the event_id tag, in field order) and bind one pointer per
      // column to the reader's default entry; LoadEntry() will refresh them.
      ROOT::REntry const& entry = prod.reader->GetModel().GetDefaultEntry();
      for (auto const& field : prod.reader->GetDescriptor().GetTopLevelFields()) {
        std::string const& name = field.GetFieldName();
        if (name == "event_id")
          continue;
        prod.columns.push_back(entry.GetPtr<float>(name));
      }

      products_.emplace(spec.name, std::move(prod));
    }

    // The index holds one row per (event, product), so the event count is the
    // number of index rows divided by the number of products.
    num_events_ =
      products.empty() ? 0 : static_cast<std::uint64_t>(index_->GetEntries()) / products.size();

    // Build the per-product row-range cache: one sequential TTree scan, before
    // any timed reads. After this block the TTree is never accessed again.
    {
      std::uint64_t ev = 0, pid = 0, rs = 0, rc = 0;
      index_->SetBranchAddress("event_id",   &ev);
      index_->SetBranchAddress("product_id", &pid);
      index_->SetBranchAddress("row_start",  &rs);
      index_->SetBranchAddress("row_count",  &rc);

      std::unordered_map<std::uint64_t, Product*> by_id;
      for (auto& [n, prod] : products_)
        by_id[prod.id] = &prod;

      Long64_t const n = index_->GetEntries();
      for (Long64_t i = 0; i < n; ++i) {
        index_->GetEntry(i);
        auto it = by_id.find(pid);
        if (it != by_id.end())
          it->second->row_cache[ev] = {rs, rc};
      }
      index_->ResetBranchAddresses();
    }
  }

  EventReader::~EventReader() = default;

  EventReader::Product& EventReader::product(std::string const& name)
  {
    auto it = products_.find(name);
    if (it == products_.end())
      throw std::runtime_error("EventReader: unknown product \"" + name + "\"");
    return it->second;
  }

  EventReader::RowRange EventReader::locate(std::uint64_t event_id, std::string const& name)
  {
    Product& p = product(name);
    auto it = p.row_cache.find(event_id);
    if (it == p.row_cache.end())
      throw std::runtime_error("EventReader: event " + std::to_string(event_id) +
                               " not found for product \"" + name + "\"");
    return it->second;
  }

  std::vector<float> EventReader::read_product(std::uint64_t event_id, std::string const& name)
  {
    using clock = std::chrono::steady_clock;
    auto const ns = [](auto d) { return std::chrono::duration<double, std::nano>(d).count(); };

    Product& p = product(name);

    clock::time_point t0;
    if (instrument_) t0 = clock::now();
    RowRange rows = locate(event_id, name);
    if (instrument_) timers_.locate_ns += ns(clock::now() - t0);

    clock::time_point tf;
    if (instrument_) tf = clock::now();
    std::vector<float> out;
    out.reserve(rows.count * p.columns.size());
    if (instrument_) timers_.fill_ns += ns(clock::now() - tf);

    for (std::uint64_t r = rows.start; r < rows.start + rows.count; ++r) {
      clock::time_point tl;
      if (instrument_) tl = clock::now();
      p.reader->LoadEntry(r); // whole-row read: refreshes every bound column pointer
      if (instrument_) timers_.load_ns += ns(clock::now() - tl);

      clock::time_point tp;
      if (instrument_) tp = clock::now();
      for (std::shared_ptr<float> const& column : p.columns)
        out.push_back(*column);
      if (instrument_) timers_.fill_ns += ns(clock::now() - tp);
    }
    return out;
  }

  std::vector<float> EventReader::read_field(std::uint64_t event_id,
                                             std::string const& name,
                                             std::string const& field)
  {
    Product& p = product(name);
    RowRange rows = locate(event_id, name);

    // Column projection: read just this one field across the event's rows.
    ROOT::RNTupleView<float> view = p.reader->GetView<float>(field);
    std::vector<float> out;
    out.reserve(rows.count);
    for (std::uint64_t r = rows.start; r < rows.start + rows.count; ++r)
      out.push_back(view(r));
    return out;
  }

  void EventReader::print_metrics() const { print_metrics(std::cout); }

  void EventReader::print_metrics(std::ostream& os) const
  {
    for (auto const& [name, p] : products_) {
      os << "=== metrics: " << name << " ===\n";
      p.reader->PrintInfo(ROOT::ENTupleInfo::kMetrics, os);
    }
  }

}
