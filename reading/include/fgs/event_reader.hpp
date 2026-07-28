#pragma once

// EventReader: the read-side counterpart of the write strategies. Given a single
// ROOT file, it reads that file's index TTree (event_id, index_value) to learn,
// for each event, which container (RNTuple) and entry hold each product, then
// returns a product's row as a flat float buffer.
//
// The reader is index-driven: it discovers the products and their containers
// from the index itself, so it knows nothing about strategy layout, variants, or
// the manifest. Picking the file (e.g. no-shuffle vs shuffle) is the caller's
// job. The container's element type is discovered from its RNTuple descriptor.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <ROOT/RNTupleReadOptions.hxx>
#include <ROOT/RNTupleReader.hxx>

#include "fgs/token.hpp"
#include "fgs/types.hpp"

namespace fgs {

  class EventReader {
  public:
    // Opens `root_file` and its index TTree (`index_name`), builds the in-memory
    // event -> tokens map, and opens one RNTuple reader per product container.
    // `opts` controls the read path (cluster cache / prefetch, metrics) and is
    // forwarded to every container reader.
    EventReader(std::filesystem::path const& root_file,
                std::string const& index_name,
                ROOT::RNTupleReadOptions opts = {});

    ~EventReader();

    EventReader(EventReader const&) = delete;
    EventReader& operator=(EventReader const&) = delete;

    std::uint64_t num_events() const { return num_events_; }

    // Product names present in the index, sorted. Callers iterate this instead of
    // hard-coding which products a file holds.
    std::vector<std::string> const& product_names() const { return product_names_; }

    // Splits the read loop's "Other" wall time (what RNTuple's read/unzip counters
    // miss) into locate lookup, LoadEntry decode, and per-event vector fill. ns.
    struct SubTimers {
      double locate_ns = 0.0;
      double load_ns = 0.0;
      double fill_ns = 0.0;
    };

    // Off by default so the timed pass pays no clock-read overhead; the runner
    // flips it on for a separate instrumented pass.
    void set_instrument(bool on) { instrument_ = on; }
    SubTimers const& subtimers() const { return timers_; }

    // Read one product's row for `event_id` as a flat float buffer (3 floats per
    // particle, in field order), via a whole-row LoadEntry. Empty if the event
    // has no particles. Throws if the event or product is unknown.
    std::vector<float> read_product(std::uint64_t event_id, std::string const& product);

    // Print ROOT's per-container performance counters (no-op unless metrics were
    // enabled in the read options). The stream overload writes to an arbitrary
    // sink; the no-arg version writes to std::cout.
    void print_metrics() const;
    void print_metrics(std::ostream& os) const;

  private:
    // One opened product container and the entry pointer bound to its vector
    // field. Exactly one of pos/mom is set, per the field's element type.
    struct Container {
      std::unique_ptr<ROOT::RNTupleReader> reader;
      std::shared_ptr<std::vector<Position>> pos;
      std::shared_ptr<std::vector<Momentum>> mom;
    };

    // Open a container reader and bind its vector field, or return the already
    // open one. Keyed by container name.
    Container& container_for(std::string const& name);

    // O(1) token lookup via the in-memory index built once in the constructor.
    // Throws if event_id or product was not present in the index.
    Token const& locate(std::uint64_t event_id, std::string const& product);

    bool instrument_ = false;
    SubTimers timers_;

    std::filesystem::path root_file_;
    ROOT::RNTupleReadOptions opts_;

    std::uint64_t num_events_ = 0;
    std::vector<std::string> product_names_;

    // event_id -> per-product tokens. Populated by one sequential index scan in
    // the constructor; the index TTree is never touched again after that.
    std::unordered_map<std::uint64_t, EventIndex> index_;
    std::map<std::string, Container> containers_;
  };

}
