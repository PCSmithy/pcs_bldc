add_library(lib_timer STATIC
  lib_timer.c
)
target_include_directories(lib_timer PUBLIC .)
target_link_libraries(lib_timer PUBLIC lib_types lib_utils)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
