#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "fgs/types.hpp"

namespace fgs {

  inline constexpr char kMagic[4] = {'F', 'G', 'S', '\0'};
  inline constexpr std::uint32_t kVersion = 2;
  inline constexpr std::size_t kProductNameSize = 32;
  inline constexpr std::size_t kProductTypeSize = 32;
  inline constexpr std::size_t kFileHeaderBytes = 80;

  // RAII writer
  class ProductWriter {
  public:
    ProductWriter(std::filesystem::path const& path,
                  std::string_view product_name,
                  std::string_view product_type,
                  std::uint64_t num_events);

    // When n_particles == 0, writes exactly 4 bytes (the uint32 zero).
    void write_event(std::uint32_t n_particles, float const* data);

    ~ProductWriter();

    ProductWriter(ProductWriter const&) = delete;
    ProductWriter& operator=(ProductWriter const&) = delete;

  private:
    std::ofstream stream_;
  };

  class ProductReader {
  public:
    explicit ProductReader(std::filesystem::path const& path);

    std::uint64_t num_events() const { return num_events_; }
    std::string product_name() const { return product_name_; }
    std::string product_type() const { return product_type_; }

    // Reads the next event record. Fills `n_particles` and `floats`
    // (n_particles * 3 values). Returns false when all events are exhausted.
    // Throws std::runtime_error on truncation or malformed data.
    bool read_next(std::uint32_t& n_particles, std::vector<float>& floats);

    ~ProductReader() = default;

  private:
    std::ifstream stream_;
    std::uint64_t num_events_ = 0;
    std::uint64_t events_read_ = 0;
    std::string product_name_;
    std::string product_type_;
  };

  // Reconstruct one Event from the raw float buffers returned by two
  // ProductReader::read_next() calls (one for positions, one for momenta).
  // pos_floats and mom_floats must each have exactly n_particles * 3 values.
  // This is the read-side inverse of the write path in run_generation().
  Event reconstruct_event(std::uint64_t event_id,
                          std::uint32_t n_particles,
                          std::vector<float> const& pos_floats,
                          std::vector<float> const& mom_floats);

  // Load every event from a completed generation output directory into memory.
  // Opens positions.bin and momenta.bin in lockstep; event order comes from the
  // files' own num_events field. Returns events in event-id order (0-based).
  // Call this before starting a benchmark timer so .bin I/O is not measured.
  std::vector<Event> load_all_events(std::filesystem::path const& output_dir);

  // Load one data product independently (e.g. positions alone), as DUNE treats
  // data products separately. Opens gen_dir/file_name (e.g. "positions.bin") and
  // returns its events in order: result[e] is event e's floats, 3 per particle.
  // Does not reorder. Call before a benchmark timer so .bin I/O is excluded.
  std::vector<std::vector<float>> load_product(std::filesystem::path const& gen_dir,
                                               std::string const& file_name);

}
