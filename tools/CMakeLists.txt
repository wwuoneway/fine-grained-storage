add_executable(fgs_inspect_columns inspect_columns/main.cpp)
target_link_libraries(fgs_inspect_columns PRIVATE
  ROOT::RIO
  ROOT::ROOTNTuple
  ROOT::ROOTNTupleUtil   # RNTupleInspector: per-column page counts
  nlohmann_json::nlohmann_json
)
target_compile_features(fgs_inspect_columns PRIVATE cxx_std_23)
