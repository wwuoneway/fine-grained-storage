#pragma once

// These are the ONLY data products in this benchmark: two real physics objects
// (a 3D position and a 3D momentum) grouped per simulated collision event.
//
// Any type persisted to a ROOT file must also be listed in dictionary/LinkDef.h
// so ROOT can generate its I/O reflection. Add a product here, add it there too.

#include <cstdint>
#include <vector>

namespace fgs {

  struct Position {
    float x, y, z;
  };

  struct Momentum {
    float px, py, pz;
  };

  struct Event {
    std::uint64_t id;
    std::vector<Position> positions; // size N
    std::vector<Momentum> momenta;   // size N (parallel to positions)

    std::size_t n_particles() const { return positions.size(); }
  };

}
