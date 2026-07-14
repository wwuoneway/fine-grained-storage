add_executable(fgs_read_bench
  main.cpp
  bench_config.cpp
  os_cache.cpp
  bench_runner.cpp
  bench_report.cpp)
target_link_libraries(fgs_read_bench PRIVATE fgs_reading)
