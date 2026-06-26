find_package(ROOT 6.38 REQUIRED COMPONENTS RIO ROOTNTuple Tree)

# EventReader: read-side library, reused by the demo driver (and later the
# benchmark loop). Links fgs_generation for fgs::Event, reconstruct_event,
# and nlohmann/json.
add_library(fgs_reading STATIC event_reader.cpp)
target_include_directories(fgs_reading PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fgs_reading PUBLIC cxx_std_23)
target_link_libraries(fgs_reading PUBLIC
  fgs_generation
  ROOT::RIO
  ROOT::ROOTNTuple
  ROOT::Tree
)

# fgs_read: demo driver that resolves a variant from the manifest and reads it.
add_executable(fgs_read main.cpp)
target_link_libraries(fgs_read PRIVATE fgs_reading)
