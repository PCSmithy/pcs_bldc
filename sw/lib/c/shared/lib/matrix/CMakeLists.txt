add_library(lib_matrix INTERFACE)
target_include_directories(lib_matrix INTERFACE .)
target_link_libraries(lib_matrix INTERFACE lib_types lib_utils)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
