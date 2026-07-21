#pragma once

// EventReader: the read-side counterpart of the write strategies. Given a single
// ROOT file plus the product registry (which the caller reads from the writing
// manifest), it uses that file's persistent TTree index to locate an event's
// rows in the product RNTuples and returns the data as a flat float buffer.
//
// The reader knows nothing about variants, strategy layout, manifest, or which
// specific products exist: it reads the file it is given, using the registry it
// is handed. Picking the file (e.g. no-shuffle vs shuffle) and extracting the
// registry from the manifest are the caller's job. The per-product column fields
// are discovered from each RNTuple itself -- nothing about the data shape is
// hardcoded here.

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

class TFile;
class TTree;

namespace fgs {

  // One product as recorded in the writing manifest: its name (also the RNTuple
  // container name) and its numeric index minor key (manifest product_id).
  struct ProductSpec {
    std::string name;
    std::uint64_t id = 0;
  };

  class EventReader {
  public:
    // Opens `root_file`, its index TTree (`index_name`), and one RNTuple reader
    // per entry in `products`. `opts` controls the read path (cluster cache /
    // prefetch, implicit MT, metrics) and is forwarded to every product reader.
    EventReader(std::filesystem::path const& root_file,
                std::string const& index_name,
                std::vector<ProductSpec> const& products,
                ROOT::RNTupleReadOptions opts = {});

    ~EventReader();

    EventReader(EventReader const&) = delete;
    EventReader& operator=(EventReader const&) = delete;

    std::uint64_t num_events() const { return num_events_; }

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
    void reset_subtimers() { timers_ = SubTimers{}; }
    SubTimers const& subtimers() const { return timers_; }

    // Read one product's rows for `event_id` as a flat float buffer (one
    // component per column, in the RNTuple's field order), via a whole-row
    // LoadEntry. Empty if the event has no particles. Throws if the event or
    // product is unknown.
    std::vector<float> read_product(std::uint64_t event_id, std::string const& product);

    // Read a single field (column) of one product for `event_id`, via a column
    // view (projection) rather than a whole-row read -- one value per particle.
    // Throws if the event, product, or field is unknown.
    std::vector<float> read_field(std::uint64_t event_id,
                                  std::string const& product,
                                  std::string const& field);

    // Print ROOT's per-reader performance counters (no-op unless metrics were
    // enabled in the read options passed to the constructor). The stream
    // overload writes the same report to an arbitrary sink (e.g. a file); the
    // no-arg version writes to std::cout.
    void print_metrics() const;
    void print_metrics(std::ostream& os) const;

  private:
    struct RowRange { std::uint64_t start, count; };

    // Everything needed to read one product: its numeric index key, the RNTuple
    // reader, one pointer per component field, and the pre-built row-range cache
    // (populated once in the constructor by scanning the TTree sequentially).
    struct Product {
      std::uint64_t id = 0;
      std::unique_ptr<ROOT::RNTupleReader> reader;
      std::vector<std::shared_ptr<float>> columns;
      std::unordered_map<std::uint64_t, RowRange> row_cache;
    };

    Product& product(std::string const& name);

    // O(1) row-range lookup via the pre-built in-memory cache.
    // Throws if event_id was not present in the TTree at construction time.
    RowRange locate(std::uint64_t event_id, std::string const& name);

    bool instrument_ = false;
    SubTimers timers_;

    std::uint64_t num_events_ = 0;
    std::unique_ptr<TFile> file_;
    TTree* index_ = nullptr; // owned by file_; used only during construction
    std::map<std::string, Product> products_;
  };

}
