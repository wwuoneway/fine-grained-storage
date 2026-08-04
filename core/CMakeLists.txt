# Dependency-free foundation (Event, Token, small helpers). Links nothing so no
# module's heavy dependencies (ROOT, JSON) leak into another through it.
add_library(fgs_core INTERFACE)
target_include_directories(fgs_core INTERFACE include)
target_compile_features(fgs_core INTERFACE cxx_std_23)
