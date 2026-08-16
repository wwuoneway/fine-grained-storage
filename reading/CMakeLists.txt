add_library(fgs_reading STATIC event_reader.cpp)
target_include_directories(fgs_reading PUBLIC include)
target_compile_features(fgs_reading PUBLIC cxx_std_23)
target_link_libraries(fgs_reading PUBLIC
  fgs_core
  fgs_generation
  fgs_dictionary
  ROOT::RIO
  ROOT::ROOTNTuple
  ROOT::Tree
)
