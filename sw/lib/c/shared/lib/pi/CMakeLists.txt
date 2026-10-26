add_library(lib_pi INTERFACE)
target_include_directories(lib_pi INTERFACE .)
target_link_libraries(lib_pi INTERFACE lib_types lib_utils)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
