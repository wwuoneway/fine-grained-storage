include(FetchContent)

# nlohmann/json: single-header JSON library, fetched at configure time so the
# project has no system dependency on it.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(fgs_generation
  src/bin_io.cpp
  src/config.cpp
  src/generator.cpp
)

target_include_directories(fgs_generation PUBLIC include)
target_link_libraries(fgs_generation PUBLIC fgs_core nlohmann_json::nlohmann_json)

# Pin the standard on the target: fetched dependencies can clobber the global
# CMAKE_CXX_STANDARD.
target_compile_features(fgs_generation PUBLIC cxx_std_23)

add_executable(fgs_generate src/generate_main.cpp)
target_link_libraries(fgs_generate PRIVATE fgs_generation)
