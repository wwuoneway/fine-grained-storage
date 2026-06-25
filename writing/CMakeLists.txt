find_package(ROOT 6.38 REQUIRED COMPONENTS RIO ROOTNTuple Tree)

add_library(fgs_common STATIC common/ordering.cpp)
target_include_directories(fgs_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fgs_common PUBLIC cxx_std_23)

# --- Write strategies --------------------------------------------------------
# One executable fgs_<name> per <name>/main.cpp. Add a new strategy by dropping
# in <name>/main.cpp and appending its directory name to this list.
set(strategies
  strategy_one
)
foreach(strategy IN LISTS strategies)
  add_executable(fgs_${strategy} ${strategy}/main.cpp)
  target_link_libraries(fgs_${strategy} PRIVATE
    fgs_common              # event ordering (shared)
    fgs_generation          # load_product(), types, nlohmann/json
    ROOT::RIO
    ROOT::ROOTNTuple
    ROOT::Tree
  )
endforeach()

# --- fgs_verify: read-side reference (index lookup + round-trip cross-check) --
add_executable(fgs_verify fgs_verify.cpp)
target_link_libraries(fgs_verify PRIVATE
  fgs_generation
  ROOT::RIO
  ROOT::ROOTNTuple
  ROOT::Tree
)
