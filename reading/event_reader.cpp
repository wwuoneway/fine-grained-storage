#include "event_reader.hpp"

#include <iostream>
#include <stdexcept>
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

    std::uint64_t row_start = 0, row_count = 0;
    index_->SetBranchAddress("row_start", &row_start);
    index_->SetBranchAddress("row_count", &row_count);

    // O(log N) lookup straight to this event's index row for this product.
    Long64_t const e =
      index_->GetEntryNumberWithIndex(static_cast<Long64_t>(event_id), static_cast<Long64_t>(p.id));
    if (e < 0)
      throw std::runtime_error("EventReader: event " + std::to_string(event_id) +
                               " not found for product \"" + name + "\"");
    index_->GetEntry(e);
    return {row_start, row_count};
  }

  std::vector<float> EventReader::read_product(std::uint64_t event_id, std::string const& name)
  {
    Product& p = product(name);
    RowRange rows = locate(event_id, name);

    std::vector<float> out;
    out.reserve(rows.count * p.columns.size());
    for (std::uint64_t r = rows.start; r < rows.start + rows.count; ++r) {
      p.reader->LoadEntry(r); // whole-row read: refreshes every bound column pointer
      for (std::shared_ptr<float> const& column : p.columns)
        out.push_back(*column);
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

  void EventReader::print_metrics() const
  {
    for (auto const& [name, p] : products_) {
      std::cout << "=== metrics: " << name << " ===\n";
      p.reader->PrintInfo(ROOT::ENTupleInfo::kMetrics);
    }
  }

}
