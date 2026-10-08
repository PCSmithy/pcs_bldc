add_library(lib_transforms INTERFACE)
target_include_directories(lib_transforms INTERFACE .)
target_link_libraries(lib_transforms INTERFACE lib_types lib_utils m)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
