cmake_minimum_required(VERSION 3.20)
project(fine_grained_storage CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wno-cpp)

# One ROOT discovery for the whole project
find_package(ROOT 6.40 REQUIRED COMPONENTS RIO ROOTNTuple ROOTNTupleUtil Tree)

add_subdirectory(core)
add_subdirectory(generation)
add_subdirectory(dictionary)
add_subdirectory(writing)
add_subdirectory(reading)
add_subdirectory(benchmarks)
add_subdirectory(tools)