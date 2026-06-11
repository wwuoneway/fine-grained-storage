# ----------------------------------------------------------------------------
# generation/ -- Phase 1 data generation component.
#
# Builds:
#   - fgs_generation : a static library (types, bin_io, config, generator)
#   - fgs_generate   : the CLI executable that links the library
# ----------------------------------------------------------------------------

include(FetchContent)

# nlohmann/json: single-header JSON library, fetched at configure time so the
# project has no system dependency on it.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Library: fgs_generation -------------------------------------------------
add_library(fgs_generation
  src/bin_io.cpp
  src/config.cpp
  src/generator.cpp
)

# PUBLIC so that anything linking fgs_generation (the executable, the tests)
# also gets the include path and the JSON dependency transitively.
target_include_directories(fgs_generation PUBLIC include)
target_link_libraries(fgs_generation PUBLIC nlohmann_json::nlohmann_json)

# Require C++23 at the target level. This is more robust than the global
# CMAKE_CXX_STANDARD, which fetched dependencies can clobber. PUBLIC makes the
# requirement propagate to consumers (fgs_generate, the tests).
target_compile_features(fgs_generation PUBLIC cxx_std_23)

# --- Executable: fgs_generate ------------------------------------------------
add_executable(fgs_generate src/generate_main.cpp)
target_link_libraries(fgs_generate PRIVATE fgs_generation)
