#pragma once

#include <cmath>
#include <ctime>
#include <string>

namespace fgs {

  inline double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

  // Readable UTC timestamp (e.g. 2026-07-24 12:34:56 UTC). Stamped at the top of
  // every manifest.json so a dataset carries the moment it was generated.
  inline std::string utc_timestamp()
  {
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);
    return buf;
  }

}
