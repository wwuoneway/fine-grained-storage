#pragma once

// OS page-cache control for the read benchmark. Isolated here so the cold/warm
// eviction mechanism is a single, testable responsibility.

#include <filesystem>

namespace fgs::bench {

  // Drop a file's pages from the OS page cache (POSIX_FADV_DONTNEED) to force a
  // cold read. Advisory and POSIX-specific; frees only clean pages, so callers
  // must flush dirty pages (sync/fsync) first for a truly cold state.
  void evict_from_cache(std::filesystem::path const& path);

}
