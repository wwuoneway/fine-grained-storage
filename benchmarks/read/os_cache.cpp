#include "os_cache.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;

namespace fgs::bench {

  void evict_from_cache(fs::path const& path)
  {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("cannot open for cache eviction: " + path.string());

    int const rc = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);

    if (rc != 0)
      throw std::runtime_error("posix_fadvise(POSIX_FADV_DONTNEED) failed for " + path.string());
  }

}
