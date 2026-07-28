# ROOT dictionary for the persisted types: the product structs (RNTuple vector
# fields) and Token / EventIndex (index TTree value column). SHARED so its
# static initializers register the types with TClass when an exe loads it; the
# generated dictionary is its only translation unit, hence the explicit language.
add_library(fgs_dictionary SHARED)
set_target_properties(fgs_dictionary PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(fgs_dictionary PUBLIC cxx_std_23)

target_link_libraries(fgs_dictionary PUBLIC fgs_core ROOT::RIO ROOT::Tree ROOT::ROOTNTuple)

# root_generate_dictionary resolves the header paths below at configure time and
# cannot see fgs_core's include dir through the link genexpr, so name it directly.
target_include_directories(fgs_dictionary PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

# Nothing references a symbol from this library (its job is the static type
# registration done when it loads), so the linker would drop it under the default
# --as-needed. Force consumers to keep it as a load-time dependency.
target_link_options(fgs_dictionary INTERFACE "LINKER:--no-as-needed")

root_generate_dictionary(fgs_dictionary_dict fgs/types.hpp fgs/token.hpp
  MODULE fgs_dictionary
  LINKDEF LinkDef.h)
