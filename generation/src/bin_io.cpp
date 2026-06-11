#include "fgs/bin_io.hpp"

#include <bit>
#include <fstream>
#include <stdexcept>
#include <vector>

// Compile-time contract: Position and Momentum must be 3 contiguous floats
// with no padding, so reinterpret_cast<const float*>(vec.data()) is safe.
static_assert(sizeof(fgs::Position) == 12);
static_assert(sizeof(fgs::Momentum) == 12);

namespace fgs {

  namespace {

    // Little-endian packing helpers

    void put_u32(std::vector<std::byte>& out, std::uint32_t v)
    {
      for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }

    void put_u64(std::vector<std::byte>& out, std::uint64_t v)
    {
      for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }

    void put_f32(std::vector<std::byte>& out, float f)
    {
      put_u32(out, std::bit_cast<std::uint32_t>(f));
    }

    // Little-endian unpacking helpers

    std::uint32_t get_u32(std::byte const* data, std::size_t& pos)
    {
      std::uint32_t v = 0;
      for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(std::to_integer<unsigned>(data[pos + i])) << (8 * i);
      pos += 4;
      return v;
    }

    std::uint64_t get_u64(std::byte const* data, std::size_t& pos)
    {
      std::uint64_t v = 0;
      for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[pos + i])) << (8 * i);
      pos += 8;
      return v;
    }

    // Fixed-width string helpers

    // Appends exactly `field_bytes` bytes to `out`: the characters of `s` followed
    // by zero bytes. Throws if s is longer than field_bytes.
    void put_padded_string(std::vector<std::byte>& out, std::string_view s, std::size_t field_bytes)
    {
      if (s.size() > field_bytes)
        throw std::runtime_error("bin_io: product name/type string exceeds " +
                                 std::to_string(field_bytes) + " bytes: \"" + std::string(s) +
                                 "\"");
      for (char c : s)
        out.push_back(static_cast<std::byte>(c));
      for (std::size_t i = s.size(); i < field_bytes; ++i)
        out.push_back(std::byte{0});
    }

  } // namespace

  // --- ProductWriter ----------------------------------------------------------

  ProductWriter::ProductWriter(std::filesystem::path const& path,
                               std::string_view product_name,
                               std::string_view product_type,
                               std::uint64_t num_events)
  {
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_)
      throw std::runtime_error("ProductWriter: cannot open " + path.string());

    // Build the 80-byte header in a buffer, then write it in one shot.
    std::vector<std::byte> hdr;
    hdr.reserve(kFileHeaderBytes);

    for (char c : kMagic)
      hdr.push_back(static_cast<std::byte>(c));             // [0..3]
    put_u32(hdr, kVersion);                                 // [4..7]
    put_padded_string(hdr, product_name, kProductNameSize); // [8..39]
    put_padded_string(hdr, product_type, kProductTypeSize); // [40..71]
    put_u64(hdr, num_events);                               // [72..79]

    stream_.write(reinterpret_cast<char const*>(hdr.data()),
                  static_cast<std::streamsize>(hdr.size()));
    if (!stream_)
      throw std::runtime_error("ProductWriter: header write failed for " + path.string());
  }

  void ProductWriter::write_event(std::uint32_t n_particles, float const* data)
  {
    // Record: [4 bytes n_particles] + [n_particles * 12 bytes floats].
    // When n_particles == 0, the buffer is 4 bytes and the loop below is a no-op.
    std::vector<std::byte> buf;
    buf.reserve(4 + std::size_t{12} * n_particles);

    put_u32(buf, n_particles);
    for (std::uint32_t i = 0; i < n_particles * 3; ++i)
      put_f32(buf, data[i]);

    stream_.write(reinterpret_cast<char const*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
    if (!stream_)
      throw std::runtime_error("ProductWriter: write_event failed");
  }

  ProductWriter::~ProductWriter() { stream_.flush(); }

  // --- ProductReader ----------------------------------------------------------

  ProductReader::ProductReader(std::filesystem::path const& path)
  {
    stream_.open(path, std::ios::binary);
    if (!stream_)
      throw std::runtime_error("ProductReader: cannot open " + path.string());

    // Read and validate the 80-byte header.
    std::byte hdr[kFileHeaderBytes];
    stream_.read(reinterpret_cast<char*>(hdr), static_cast<std::streamsize>(kFileHeaderBytes));
    if (!stream_)
      throw std::runtime_error("ProductReader: file smaller than header: " + path.string());

    // Verify magic.
    for (int i = 0; i < 4; ++i) {
      if (static_cast<char>(hdr[i]) != kMagic[i])
        throw std::runtime_error("ProductReader: bad magic in " + path.string());
    }

    // Verify version.
    std::size_t pos = 4;
    std::uint32_t const version = get_u32(hdr, pos);
    if (version == 1)
      throw std::runtime_error("ProductReader: FGS1 per-event format detected in " + path.string() +
                               " — re-run generation to produce FGS2 product files");
    if (version != kVersion)
      throw std::runtime_error("ProductReader: unsupported version " + std::to_string(version) +
                               " in " + path.string());

    product_name_.assign(reinterpret_cast<char const*>(hdr + 8), kProductNameSize);
    {
      std::size_t const end = product_name_.find_last_not_of('\0');
      product_name_ = (end == std::string::npos) ? std::string{} : product_name_.substr(0, end + 1);
    }
    product_type_.assign(reinterpret_cast<char const*>(hdr + 40), kProductTypeSize);
    {
      std::size_t const end = product_type_.find_last_not_of('\0');
      product_type_ = (end == std::string::npos) ? std::string{} : product_type_.substr(0, end + 1);
    }

    pos = 72;
    num_events_ = get_u64(hdr, pos);
  }

  bool ProductReader::read_next(std::uint32_t& n_particles, std::vector<float>& floats)
  {
    if (events_read_ == num_events_)
      return false;

    // Read the 4-byte particle count.
    std::byte count_buf[4];
    stream_.read(reinterpret_cast<char*>(count_buf), 4);
    if (stream_.eof() && stream_.gcount() == 0 && events_read_ == num_events_)
      return false;
    if (!stream_ || stream_.gcount() != 4)
      throw std::runtime_error("ProductReader: truncated n_particles record");

    std::size_t pos = 0;
    n_particles = get_u32(count_buf, pos);

    // Read n_particles * 3 floats.
    floats.resize(n_particles * 3);
    if (n_particles > 0) {
      std::size_t const float_bytes = std::size_t{12} * n_particles;
      std::vector<std::byte> data_buf(float_bytes);
      stream_.read(reinterpret_cast<char*>(data_buf.data()),
                   static_cast<std::streamsize>(float_bytes));
      if (!stream_ || static_cast<std::size_t>(stream_.gcount()) != float_bytes)
        throw std::runtime_error("ProductReader: truncated float data");

      pos = 0;
      for (std::uint32_t i = 0; i < n_particles * 3; ++i) {
        std::uint32_t raw = get_u32(data_buf.data(), pos);
        floats[i] = std::bit_cast<float>(raw);
      }
    }

    ++events_read_;
    return true;
  }

  // Free functions

  Event reconstruct_event(std::uint64_t event_id,
                          std::uint32_t n_particles,
                          std::vector<float> const& pos_floats,
                          std::vector<float> const& mom_floats)
  {
    Event e;
    e.id = event_id;
    e.positions.resize(n_particles);
    e.momenta.resize(n_particles);
    for (std::uint32_t i = 0; i < n_particles; ++i) {
      e.positions[i] = {pos_floats[3 * i], pos_floats[3 * i + 1], pos_floats[3 * i + 2]};
      e.momenta[i] = {mom_floats[3 * i], mom_floats[3 * i + 1], mom_floats[3 * i + 2]};
    }
    return e;
  }

  std::vector<Event> load_all_events(std::filesystem::path const& output_dir)
  {
    namespace fs = std::filesystem;

    // Open both product files in lockstep; the header num_events field drives iteration.
    ProductReader pos_reader(output_dir / "positions.bin");
    ProductReader mom_reader(output_dir / "momenta.bin");

    if (pos_reader.num_events() != mom_reader.num_events())
      throw std::runtime_error(
        "load_all_events: positions.bin and momenta.bin have different event counts");

    std::uint64_t const num_events = pos_reader.num_events();
    std::vector<Event> events;
    events.reserve(num_events);

    std::vector<float> pos_floats, mom_floats;
    std::uint32_t pos_n = 0, mom_n = 0;

    for (std::uint64_t id = 0; id < num_events; ++id) {
      if (!pos_reader.read_next(pos_n, pos_floats) || !mom_reader.read_next(mom_n, mom_floats))
        throw std::runtime_error("load_all_events: unexpected end of product file");

      if (pos_n != mom_n)
        throw std::runtime_error(
          "load_all_events: n_particles mismatch between product files at event " +
          std::to_string(id));

      events.push_back(reconstruct_event(id, pos_n, pos_floats, mom_floats));
    }

    return events;
  }

}
