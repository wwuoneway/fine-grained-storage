#pragma once

// EventReader: the read-side counterpart of the write strategies. Given a single
// ROOT file, it reads that file's index TTree (event_id, index_value) to learn,
// for each event, which container (RNTuple) and entry hold each product, then
// reads a product's row.
//
// The reader is index-driven: it discovers the products and their containers
// from the index itself, so it knows nothing about strategy layout, variants, or
// the manifest. Picking the file (e.g. no-shuffle vs shuffle) is the caller's
// job. The container's element type is discovered from its RNTuple descriptor.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <ROOT/RNTupleReader.hxx>

#include "fgs/types.hpp"

namespace fgs {

  class EventReader {
  public:
    // Opens `root_file` and its index TTree (`index_name`), builds the in-memory
    // index, and opens one RNTuple reader per product container.
    // `opts` controls the read path (cluster cache / prefetch, metrics) and is
    // forwarded to every container reader. An empty `products` reads every
    // product in the index; otherwise only those, and only their containers
    // are opened.
    EventReader(std::filesystem::path const& root_file,
                std::string const& index_name,
                ROOT::RNTupleReadOptions opts = {},
                std::vector<std::string> const& products = {});

    ~EventReader();

    EventReader(EventReader const&) = delete;
    EventReader& operator=(EventReader const&) = delete;

    std::uint64_t num_events() const { return num_events_; }

    // The products this reader reads, sorted. Callers iterate this instead of
    // hard-coding which products a file holds.
    std::vector<std::string> const& product_names() const { return product_names_; }

    // Decompressed bytes one element occupies, from the storage width of its
    // container's payload columns. Parallel to product_names().
    std::vector<std::size_t> const& product_element_bytes() const { return product_element_bytes_; }

    // Read one product's row for `event_id` via a whole-row LoadEntry, leaving the
    // values in the container's own buffer. Returns how many elements the row
    // holds. Throws if the event or product is unknown.
    std::size_t read_product(std::uint64_t event_id, std::string const& product);

    // Print ROOT's per-container performance counters (no-op unless metrics were
    // enabled in the read options). The stream overload writes to an arbitrary
    // sink; the no-arg version writes to std::cout.
    void print_metrics() const;
    void print_metrics(std::ostream& os) const;

    // A container's shape on disk: cluster count and total on-disk page count
    // across all its physical columns. Fixed regardless of how it's later read.
    struct ContainerFacts {
      std::uint64_t clusters = 0;
      std::uint64_t pages = 0;
    };

    // One entry per open container, keyed by container name.
    std::map<std::string, ContainerFacts> dataset_facts() const;

  private:
    // One opened product container and the entry pointer bound to its vector
    // field. Exactly one of pos/mom is set, per the field's element type.
    struct Container {
      std::unique_ptr<ROOT::RNTupleReader> reader;
      std::shared_ptr<std::vector<Position>> pos;
      std::shared_ptr<std::vector<Momentum>> mom;
      std::size_t element_bytes = 0;
    };

    // Open a container reader and bind its vector field, or return the already
    // open one. Keyed by container name.
    Container& container_for(std::string const& name);

    struct Location {
      std::uint64_t entry = 0;
      std::size_t product = 0;
    };

    // O(1) lookup in the in-memory index built once in the constructor.
    // Throws if event_id or product was not present in the index.
    Location locate(std::uint64_t event_id, std::string const& product) const;

    std::filesystem::path root_file_;
    ROOT::RNTupleReadOptions opts_;

    std::uint64_t num_events_ = 0;
    std::vector<std::string> product_names_;

    // Row numbers only, at entries_[event_id * product_names_.size() + product];
    // a product always lives in one container, so that is held per product.
    std::vector<std::uint64_t> entries_;
    std::vector<Container*> product_containers_;
    std::vector<std::size_t> product_element_bytes_;

    std::map<std::string, Container> containers_;
  };

}
