// fgs_inspect_columns: record the per-column page count of every RNTuple a read
// benchmark touched.
//
// A page count is only knowable from the written file, and the plotting layer
// needs it to put "events per page" on an axis. This walks run directories the
// read benchmark produced, takes each one's root_file and containers from its
// metadata.txt, and writes <run_dir>/columns.json:
//
//   { "<root file>": { "<container>": { "<field>": <pages>, ... } } }
//
// Usage: fgs_inspect_columns <run_dir>...

#include <ROOT/RNTupleInspector.hxx>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
  using json = nlohmann::ordered_json;

  std::string trim(std::string s)
  {
    auto const not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
  }

  std::vector<std::string> split(std::string const& s, char sep)
  {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
      std::size_t const hit = s.find(sep, start);
      std::size_t const end = hit == std::string::npos ? s.size() : hit;
      if (end > start)
        out.push_back(s.substr(start, end - start));
      if (hit == std::string::npos)
        break;
      start = hit + 1;
    }
    return out;
  }

  // Top-level "key : value" pairs only. An indented line belongs to a nested
  // block (dataset_facts) and carries no key of its own.
  std::map<std::string, std::string> read_metadata(fs::path const& path)
  {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || std::isspace(static_cast<unsigned char>(line.front())))
        continue;
      std::size_t const colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      out[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }
    return out;
  }

  // A field can own several physical columns; the largest is the one that sets
  // how much of the file a single page covers.
  std::map<std::string, std::uint64_t> column_pages(std::string const& container,
                                                    std::string const& root_file)
  {
    auto insp = ROOT::Experimental::RNTupleInspector::Create(container, root_file);
    ROOT::RNTupleDescriptor const& desc = insp->GetDescriptor();

    std::map<std::string, std::uint64_t> out;
    for (auto const& col : desc.GetColumnIterable()) {
      if (col.IsAliasColumn())
        continue;
      auto const pages =
        static_cast<std::uint64_t>(insp->GetColumnInspector(col.GetPhysicalId()).GetNPages());
      std::uint64_t& slot = out[desc.GetQualifiedFieldName(col.GetFieldId())];
      slot = std::max(slot, pages);
    }
    return out;
  }

  // root file -> the containers some benchmark in this run read from it.
  std::map<std::string, std::set<std::string>> containers_read(fs::path const& run_dir)
  {
    std::map<std::string, std::set<std::string>> out;
    fs::path const bench_root = run_dir / "benchmarks";
    if (!fs::is_directory(bench_root))
      return out;

    std::vector<fs::path> bench_dirs;
    for (auto const& entry : fs::directory_iterator(bench_root))
      if (entry.is_directory())
        bench_dirs.push_back(entry.path());
    std::sort(bench_dirs.begin(), bench_dirs.end());

    for (auto const& dir : bench_dirs) {
      fs::path const meta = dir / "metadata.txt";
      if (!fs::exists(meta))
        continue;
      auto const fields = read_metadata(meta);
      auto const root_file = fields.find("root_file");
      auto const containers = fields.find("containers");
      if (root_file == fields.end() || containers == fields.end())
        continue;
      for (auto const& name : split(containers->second, '|'))
        out[root_file->second].insert(name);
    }
    return out;
  }

  bool inspect_run(fs::path const& run_dir)
  {
    auto const wanted = containers_read(run_dir);
    if (wanted.empty()) {
      std::cerr << "fgs_inspect_columns: no benchmark metadata under " << run_dir << "\n";
      return false;
    }

    json out = json::object();
    for (auto const& [root_file, containers] : wanted) {
      json file_node = json::object();
      for (auto const& container : containers) {
        try {
          file_node[container] = column_pages(container, root_file);
        }
        catch (std::exception const& e) {
          std::cerr << "fgs_inspect_columns: cannot inspect " << container << " in " << root_file
                    << ": " << e.what() << "\n";
        }
      }
      if (!file_node.empty())
        out[root_file] = std::move(file_node);
    }
    if (out.empty())
      return false;

    fs::path const path = run_dir / "columns.json";
    std::ofstream(path) << out.dump(2) << "\n";
    std::cout << "columns -> " << path.string() << "\n";
    return true;
  }
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::cerr << "usage: fgs_inspect_columns <run_dir>...\n";
    return 2;
  }

  bool ok = true;
  for (int i = 1; i < argc; ++i)
    ok = inspect_run(fs::path(argv[i])) && ok;
  return ok ? 0 : 1;
}
