add_library(lib_filterIIR STATIC
  lib_filterIIR.c
)
target_include_directories(lib_filterIIR PUBLIC .)
target_link_libraries(lib_filterIIR PUBLIC lib_types lib_utils)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
