add_library(fgs_writing STATIC ordering.cpp)
target_include_directories(fgs_writing PUBLIC include)
target_compile_features(fgs_writing PUBLIC cxx_std_23)

# One executable fgs_<name> per <name>/main.cpp. Add a new strategy by dropping
# in <name>/main.cpp and appending its directory name to this list.
set(strategies
  strategy_one
)
foreach(strategy IN LISTS strategies)
  add_executable(fgs_${strategy} ${strategy}/main.cpp)
  target_link_libraries(fgs_${strategy} PRIVATE
    fgs_core                # Event/Position/Momentum, Token, util helpers
    fgs_writing             # event write-ordering helper
    fgs_generation          # load_product(), nlohmann/json
    fgs_dictionary          # ROOT reflection for the persisted types
    ROOT::RIO
    ROOT::ROOTNTuple
    ROOT::ROOTNTupleUtil   # RNTupleInspector: page/cluster counts after writing
    ROOT::Tree
  )
endforeach()

# fgs_verify: read-side reference (index lookup + round-trip cross-check).
add_executable(fgs_verify fgs_verify.cpp)
target_link_libraries(fgs_verify PRIVATE
  fgs_core
  fgs_generation
  fgs_dictionary
  ROOT::RIO
  ROOT::ROOTNTuple
  ROOT::Tree
)
