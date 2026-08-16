#include "bench_report.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <sys/utsname.h>
#include <unistd.h>

#include <ROOT/RVersion.hxx>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace fgs::bench {

  namespace {

    // Both units, because ROOT's option is bytes but the study axis is MiB.
    std::string page_size_text(std::uint64_t bytes)
    {
      if (bytes == 0)
        return "unknown (file written before this was recorded)";
      std::ostringstream os;
      os << bytes << " bytes (" << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB)";
      return os.str();
    }

    std::string trim(std::string const& s)
    {
      std::size_t const a = s.find_first_not_of(" \t");
      std::size_t const b = s.find_last_not_of(" \t");
      return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
    }

    // Parse an int, returning -1 when the value is missing or non-numeric, so a
    // malformed /proc/cpuinfo line is skipped rather than aborting the whole run.
    int parse_int(std::string const& s)
    {
      try {
        return std::stoi(s);
      } catch (...) {
        return -1;
      }
    }

    // Escape a string for a CSV field (RFC 4180): quote it when it contains a
    // comma, quote, or newline, doubling any embedded quote. Numeric columns
    // never need this; freeform text (benchmark names, paths) does.
    std::string csv_field(std::string const& s)
    {
      if (s.find_first_of(",\"\n\r") == std::string::npos)
        return s;
      std::string out = "\"";
      for (char const c : s) {
        if (c == '"')
          out += '"';
        out += c;
      }
      out += '"';
      return out;
    }

    // Value after the first ':' on a /proc/cpuinfo line, trimmed.
    std::string field_value(std::string const& line)
    {
      std::size_t const colon = line.find(':');
      return colon == std::string::npos ? std::string{} : trim(line.substr(colon + 1));
    }

    // Human-friendly rendering of a raw counter value: nanoseconds -> ms (/1e6)
    // and bytes -> MiB (/2^20, matching the writing side's power-of-two unit).
    // Other units get no conversion.
    std::string humanize(std::string const& unit, std::string const& value)
    {
      double v = 0.0;
      try {
        v = std::stod(value);
      } catch (...) {
        return {};
      }
      std::ostringstream o;
      o << std::fixed << std::setprecision(3);
      if (unit == "ns")
        o << v / 1e6 << " ms";
      else if (unit == "B")
        o << v / (1024.0 * 1024.0) << " MiB";
      else
        return {};
      return o.str();
    }

    std::string format_time(char const* fmt)
    {
      std::time_t const t = std::time(nullptr);
      std::tm tm{};
      ::localtime_r(&t, &tm);
      char buf[32];
      std::strftime(buf, sizeof(buf), fmt, &tm);
      return buf;
    }

    // Aggregate statistics over a benchmark's repetitions (assumes >= 1 rep,
    // which parse_benchmark enforces).
    struct Aggregates {
      std::size_t reps = 0;
      double wall_mean = 0.0, wall_min = 0.0;
      double lat_mean = 0.0, lat_min = 0.0;
      double thr_mean = 0.0;
      double read_ms_mean = 0.0, unzip_ms_mean = 0.0, payload_mib_mean = 0.0;
      double n_read_mean = 0.0, read_eff_mean = 0.0;
      double n_page_read_mean = 0.0, n_page_unsealed_mean = 0.0, n_cluster_loaded_mean = 0.0;
      double locate_ms_mean = 0.0, load_ms_mean = 0.0;
      double wall_instr_ms_mean = 0.0;
      double read_wall_instr_ms_mean = 0.0, unzip_wall_instr_ms_mean = 0.0;
    };

    Aggregates aggregate(std::vector<Measurement> const& reps)
    {
      std::vector<double> wall, latency;
      double read_ms_sum = 0.0, unzip_ms_sum = 0.0, payload_sum = 0.0;
      double n_read_sum = 0.0, read_eff_sum = 0.0;
      double n_page_read_sum = 0.0, n_page_unsealed_sum = 0.0, n_cluster_loaded_sum = 0.0;
      double locate_sum = 0.0, load_sum = 0.0;
      double wall_instr_sum = 0.0;
      double read_instr_sum = 0.0, unzip_instr_sum = 0.0;
      for (Measurement const& m : reps) {
        wall.push_back(m.wall_s);
        latency.push_back(m.latency_us_per_event);
        read_ms_sum += m.counters.read_wall_ms;
        unzip_ms_sum += m.counters.unzip_wall_ms;
        payload_sum += m.counters.read_payload_mib;
        n_read_sum += static_cast<double>(m.counters.n_read);
        read_eff_sum += m.counters.read_efficiency;
        n_page_read_sum += static_cast<double>(m.counters.n_page_read);
        n_page_unsealed_sum += static_cast<double>(m.counters.n_page_unsealed);
        n_cluster_loaded_sum += static_cast<double>(m.counters.n_cluster_loaded);
        locate_sum += m.locate_ms;
        load_sum += m.load_ms;
        wall_instr_sum += m.wall_instr_ms;
        read_instr_sum += m.read_wall_instr_ms;
        unzip_instr_sum += m.unzip_wall_instr_ms;
      }
      auto const n = static_cast<double>(reps.size());
      double const wall_mean = std::accumulate(wall.begin(), wall.end(), 0.0) / n;
      double const lat_mean = std::accumulate(latency.begin(), latency.end(), 0.0) / n;
      // Derived from mean latency; averaging per-rep rates would overweight
      // fast reps and disagree with latency_us_mean in the same row.
      double const thr_mean = lat_mean > 0.0 ? 1.0e6 / lat_mean : 0.0;
      return {reps.size(),
              wall_mean,
              *std::min_element(wall.begin(), wall.end()),
              lat_mean,
              *std::min_element(latency.begin(), latency.end()),
              thr_mean,
              read_ms_sum / n,
              unzip_ms_sum / n,
              payload_sum / n,
              n_read_sum / n,
              read_eff_sum / n,
              n_page_read_sum / n,
              n_page_unsealed_sum / n,
              n_cluster_loaded_sum / n,
              locate_sum / n,
              load_sum / n,
              wall_instr_sum / n,
              read_instr_sum / n,
              unzip_instr_sum / n};
    }

  }

  ReadCounters parse_read_counters(std::string const& raw_dump)
  {
    // Sum the raw volumes/times across every product section, then derive ratios.
    double payload_b = 0.0, overhead_b = 0.0, wall_read_ns = 0.0, wall_unzip_ns = 0.0;
    std::uint64_t n_read = 0;
    std::uint64_t n_page_read = 0, n_page_unsealed = 0, n_cluster_loaded = 0;

    std::istringstream in(raw_dump);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line.rfind("===", 0) == 0)
        continue;
      // Raw line: <full.counter.name>|<unit>|<description>|<value>
      std::vector<std::string> parts;
      std::size_t prev = 0, pos;
      while ((pos = line.find('|', prev)) != std::string::npos) {
        parts.push_back(line.substr(prev, pos - prev));
        prev = pos + 1;
      }
      parts.push_back(line.substr(prev));
      if (parts.size() < 4)
        continue;

      std::string const& full = parts.front();
      std::string const name = full.substr(full.find_last_of('.') + 1);
      double value = 0.0;
      try {
        value = std::stod(parts.back());
      } catch (...) {
        continue;
      }

      if (name == "szReadPayload")
        payload_b += value;
      else if (name == "szReadOverhead")
        overhead_b += value;
      else if (name == "timeWallRead")
        wall_read_ns += value;
      else if (name == "timeWallUnzip")
        wall_unzip_ns += value;
      else if (name == "nRead")
        n_read += static_cast<std::uint64_t>(value);
      else if (name == "nPageRead")
        n_page_read += static_cast<std::uint64_t>(value);
      else if (name == "nPageUnsealed")
        n_page_unsealed += static_cast<std::uint64_t>(value);
      else if (name == "nClusterLoaded")
        n_cluster_loaded += static_cast<std::uint64_t>(value);
    }

    ReadCounters c;
    c.read_wall_ms = wall_read_ns / 1e6;
    c.unzip_wall_ms = wall_unzip_ns / 1e6;
    c.read_payload_mib = payload_b / (1024.0 * 1024.0);
    c.n_read = n_read;
    c.n_page_read = n_page_read;
    c.n_page_unsealed = n_page_unsealed;
    c.n_cluster_loaded = n_cluster_loaded;
    double const total = payload_b + overhead_b;
    if (total > 0.0)
      c.read_efficiency = payload_b / total;
    return c;
  }

  std::string timestamp_now() { return format_time("%Y%m%d-%H%M%S"); }

  std::string timestamp_human() { return format_time("%Y-%m-%d %H:%M:%S"); }

  nlohmann::json system_info()
  {
    nlohmann::json j;

    char host[256];
    if (::gethostname(host, sizeof(host)) == 0) {
      host[sizeof(host) - 1] = '\0';
      j["hostname"] = host;
    } else {
      j["hostname"] = "unknown";
    }

    // CPU model and physical-core count from /proc/cpuinfo. Physical cores are
    // the unique (physical id, core id) pairs; logical cores come from sysconf.
    std::string cpu_model = "unknown";
    std::set<std::pair<int, int>> physical_cores;
    {
      std::ifstream cpuinfo("/proc/cpuinfo");
      std::string line;
      int phys = -1, core = -1;
      while (std::getline(cpuinfo, line)) {
        if (line.empty()) {
          if (phys >= 0 && core >= 0)
            physical_cores.emplace(phys, core);
          phys = core = -1;
        } else if (cpu_model == "unknown" && line.rfind("model name", 0) == 0) {
          cpu_model = field_value(line);
        } else if (line.rfind("physical id", 0) == 0) {
          phys = parse_int(field_value(line));
        } else if (line.rfind("core id", 0) == 0) {
          core = parse_int(field_value(line));
        }
      }
      if (phys >= 0 && core >= 0)
        physical_cores.emplace(phys, core);
    }

    long const logical = ::sysconf(_SC_NPROCESSORS_ONLN);
    j["cpu_model"] = cpu_model;
    j["logical_cores"] = static_cast<int>(logical);
    j["physical_cores"] =
      physical_cores.empty() ? static_cast<int>(logical) : static_cast<int>(physical_cores.size());

    long const pages = ::sysconf(_SC_PHYS_PAGES);
    long const page_size = ::sysconf(_SC_PAGE_SIZE);
    double const ram_gb = static_cast<double>(pages) * static_cast<double>(page_size) / 1e9;
    j["ram_total_gb"] = std::round(ram_gb * 100.0) / 100.0;

    utsname uts{};
    if (::uname(&uts) == 0)
      j["kernel"] = std::string(uts.sysname) + " " + uts.release + " " + uts.machine;
    else
      j["kernel"] = "unknown";

    j["root_version"] = ROOT_RELEASE;
    return j;
  }

  void write_run_info(fs::path const& path,
                      fs::path const& config_file,
                      nlohmann::json const& config)
  {
    nlohmann::json j;
    j["generated_at"] = timestamp_now();
    j["config_file"] = config_file.string();
    j["system"] = system_info();
    j["config"] = config;
    std::ofstream(path) << j.dump(2) << '\n';
  }

  char const* summary_header()
  {
    return "benchmark_num,benchmark,variant,access_pattern,scatter_distance,cache_state,"
           "cluster_cache,implicit_mt,containers,"
           "num_events,reps,wall_s_mean,wall_s_min,latency_us_mean,latency_us_min,"
           "throughput_evt_s_mean,"
           "read_wall_ms,unzip_wall_ms,read_payload_mib,n_read,read_efficiency,"
           "n_page_read,n_page_unsealed,n_cluster_loaded,"
           "locate_ms,load_ms,wall_instr_ms,read_wall_instr_ms,unzip_wall_instr_ms\n";
  }

  void write_raw_csv(fs::path const& path,
                     BenchmarkId const& id,
                     std::vector<Measurement> const& reps)
  {
    std::ofstream csv(path);
    if (!csv)
      throw std::runtime_error("cannot write CSV: " + path.string());

    csv << "benchmark_num,benchmark,variant,access_pattern,scatter_distance,cache_state,"
           "cluster_cache,implicit_mt,containers,"
           "num_events,repetition,wall_s,latency_us_per_event,throughput_evt_s,total_elements,"
           "read_wall_ms,unzip_wall_ms,read_payload_mib,n_read,read_efficiency,"
           "n_page_read,n_page_unsealed,n_cluster_loaded,"
           "locate_ms,load_ms,wall_instr_ms,read_wall_instr_ms,unzip_wall_instr_ms\n";
    for (Measurement const& m : reps)
      csv << id.num << ',' << csv_field(id.name) << ',' << csv_field(id.variant) << ','
          << csv_field(id.access_pattern) << ',' << id.scatter_distance << ','
          << csv_field(id.cache_state) << ','
          << csv_field(id.cluster_cache) << ',' << csv_field(id.implicit_mt) << ','
          << csv_field(id.containers) << ',' << id.num_events
          << ',' << m.repetition << ',' << m.wall_s << ',' << m.latency_us_per_event << ','
          << m.throughput_evt_s << ',' << m.total_elements << ','
          << m.counters.read_wall_ms << ',' << m.counters.unzip_wall_ms << ','
          << m.counters.read_payload_mib << ',' << m.counters.n_read << ','
          << m.counters.read_efficiency << ','
          << m.counters.n_page_read << ',' << m.counters.n_page_unsealed << ','
          << m.counters.n_cluster_loaded << ','
          << m.locate_ms << ',' << m.load_ms << ',' << m.wall_instr_ms << ','
          << m.read_wall_instr_ms << ',' << m.unzip_wall_instr_ms << '\n';
  }

  void append_summary_row(std::ostream& csv,
                          BenchmarkId const& id,
                          std::vector<Measurement> const& reps)
  {
    Aggregates const a = aggregate(reps);
    csv << id.num << ',' << csv_field(id.name) << ',' << csv_field(id.variant) << ','
        << csv_field(id.access_pattern) << ',' << id.scatter_distance << ','
        << csv_field(id.cache_state) << ','
        << csv_field(id.cluster_cache) << ',' << csv_field(id.implicit_mt) << ','
        << csv_field(id.containers) << ',' << id.num_events
        << ',' << a.reps << ',' << a.wall_mean << ',' << a.wall_min << ',' << a.lat_mean << ','
        << a.lat_min << ',' << a.thr_mean << ','
        << a.read_ms_mean << ',' << a.unzip_ms_mean << ',' << a.payload_mib_mean << ','
        << a.n_read_mean << ',' << a.read_eff_mean << ','
        << a.n_page_read_mean << ',' << a.n_page_unsealed_mean << ',' << a.n_cluster_loaded_mean << ','
        << a.locate_ms_mean << ',' << a.load_ms_mean << ','
        << a.wall_instr_ms_mean << ',' << a.read_wall_instr_ms_mean << ','
        << a.unzip_wall_instr_ms_mean << '\n';
  }

  void write_benchmark_metadata(fs::path const& path, BenchmarkId const& id,
                                std::map<std::string, ContainerFacts> const& dataset_facts)
  {
    std::ofstream out(path);
    if (!out)
      throw std::runtime_error("cannot write benchmark metadata: " + path.string());
    out << "benchmark_num  : " << id.num << '\n'
        << "name           : " << id.name << '\n'
        << "description    : " << id.description << '\n'
        << "variant        : " << id.variant << '\n'
        << "access_pattern : " << id.access_pattern << '\n'
        << "scatter_dist   : " << id.scatter_distance << '\n'
        << "scatter_seed   : " << id.scatter_seed << '\n'
        << "scatter_disp   : mean=" << id.scatter_mean_displacement
        << " max=" << id.scatter_max_displacement << '\n'
        << "cache_state    : " << id.cache_state << '\n'
        << "cluster_cache  : " << id.cluster_cache << '\n'
        << "containers     : " << id.containers << '\n'
        << "num_events     : " << id.num_events << '\n'
        << "repetitions    : " << id.repetitions << '\n'
        << "root_file      : " << id.root_file << '\n'
        << "manifest_file  : " << id.manifest_file << '\n'
        << "max_page_size  : " << page_size_text(id.max_page_size_bytes) << '\n';

    if (!dataset_facts.empty()) {
      out << "dataset_facts  :\n";
      for (auto const& [name, facts] : dataset_facts)
        out << "  " << name << " : clusters=" << facts.clusters << " pages=" << facts.pages << '\n';
    }
  }

  void write_benchmark_summary_txt(fs::path const& path,
                                   BenchmarkId const& id,
                                   std::vector<Measurement> const& reps)
  {
    std::ofstream out(path);
    if (!out)
      throw std::runtime_error("cannot write benchmark summary: " + path.string());

    Aggregates const a = aggregate(reps);
    out << "benchmark " << id.num << " - " << id.name << '\n'
        << "variant: " << id.variant << " | cache: " << id.cache_state
        << " | cluster_cache: " << id.cluster_cache << " | events: " << id.num_events << '\n'
        << "reps                     : " << a.reps << '\n'
        << "wall_s   mean/min        : " << a.wall_mean << " / " << a.wall_min << '\n'
        << "latency  mean/min (us/ev): " << a.lat_mean << " / " << a.lat_min << '\n'
        << "throughput mean  (evt/s) : " << a.thr_mean << '\n';
  }

  std::string format_metrics_table(std::string const& raw_dump)
  {
    if (raw_dump.empty())
      return {};

    std::ostringstream out;

    struct Row {
      std::string counter, value, unit, human, description;
    };

    std::vector<Row> rows;
    auto flush = [&] {
      if (rows.empty())
        return;
      std::size_t wc = 7, wv = 5, wu = 4, wh = 8; // "counter","value","unit","readable"
      for (Row const& r : rows) {
        wc = std::max(wc, r.counter.size());
        wv = std::max(wv, r.value.size());
        wu = std::max(wu, r.unit.size());
        wh = std::max(wh, r.human.size());
      }
      out << std::left << std::setw(static_cast<int>(wc)) << "counter" << "  " << std::right
          << std::setw(static_cast<int>(wv)) << "value" << "  " << std::left
          << std::setw(static_cast<int>(wu)) << "unit" << "  " << std::setw(static_cast<int>(wh))
          << "readable" << "  description\n";
      out << std::string(wc, '-') << "  " << std::string(wv, '-') << "  " << std::string(wu, '-')
          << "  " << std::string(wh, '-') << "  -----------\n";
      for (Row const& r : rows)
        out << std::left << std::setw(static_cast<int>(wc)) << r.counter << "  " << std::right
            << std::setw(static_cast<int>(wv)) << r.value << "  " << std::left
            << std::setw(static_cast<int>(wu)) << r.unit << "  " << std::setw(static_cast<int>(wh))
            << r.human << "  " << r.description << '\n';
      out << '\n';
      rows.clear();
    };

    std::istringstream in(raw_dump);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty())
        continue;
      if (line.rfind("===", 0) == 0) { // section header (e.g. "=== metrics: momentum ===")
        flush();
        out << line << "\n\n";
        continue;
      }

      // Line format: <long.counter.name>|<unit>|<description>|<value>
      std::vector<std::string> parts;
      std::size_t prev = 0, pos;
      while ((pos = line.find('|', prev)) != std::string::npos) {
        parts.push_back(line.substr(prev, pos - prev));
        prev = pos + 1;
      }
      parts.push_back(line.substr(prev));
      if (parts.size() < 4)
        continue;

      Row r;
      std::string const& full = parts.front();
      r.counter = full.substr(full.find_last_of('.') + 1);
      r.unit = parts[1];
      r.value = parts.back();
      r.human = humanize(r.unit, r.value);
      for (std::size_t i = 2; i + 1 < parts.size(); ++i) {
        if (i > 2)
          r.description += '|';
        r.description += parts[i];
      }
      rows.push_back(std::move(r));
    }
    flush();
    return out.str();
  }

  void write_run_report(fs::path const& path,
                        BenchmarkId const& id,
                        Measurement const& m,
                        std::string const& started_at,
                        std::string const& metrics_raw)
  {
    std::ofstream out(path);
    if (!out)
      throw std::runtime_error("cannot write run report: " + path.string());

    out << "benchmark " << id.num << " - " << id.name << "  (run " << m.repetition << ")\n"
        << "started              : " << started_at << '\n'
        << "variant              : " << id.variant << '\n'
        << "cache_state          : " << id.cache_state << '\n'
        << "cluster_cache        : " << id.cluster_cache << '\n'
        << "num_events           : " << id.num_events << '\n'
        << "max_page_size        : " << page_size_text(id.max_page_size_bytes) << '\n'
        << "wall_s               : " << m.wall_s << '\n'
        << "latency_us_per_event : " << m.latency_us_per_event << '\n'
        << "throughput_evt_s     : " << m.throughput_evt_s << '\n'
        << "total_elements       : " << m.total_elements
        << '\n'
        << "read_wall_ms         : " << m.counters.read_wall_ms << '\n'
        << "unzip_wall_ms        : " << m.counters.unzip_wall_ms << '\n'
        << "read_payload_mib     : " << m.counters.read_payload_mib << '\n'
        << "n_read               : " << m.counters.n_read << '\n'
        << "read_efficiency      : " << m.counters.read_efficiency << '\n'
        << "n_page_read          : " << m.counters.n_page_read << '\n'
        << "n_page_unsealed      : " << m.counters.n_page_unsealed << '\n'
        << "n_cluster_loaded     : " << m.counters.n_cluster_loaded << "\n\n";

    std::string const table = format_metrics_table(metrics_raw);
    if (table.empty())
      out << "(metrics not enabled for this benchmark)\n";
    else
      out << table;
  }

}
