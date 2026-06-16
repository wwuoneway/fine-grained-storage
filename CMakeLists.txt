cmake_minimum_required(VERSION 3.20)
project(fine_grained_storage CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)    

add_compile_options(-Wall -Wextra)

add_subdirectory(generation)
