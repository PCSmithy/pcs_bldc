add_library(lib_cobs STATIC
  lib_cobs.c
)
pcs_hot_path(lib_cobs)   # server path: see sw/cmake/pcs_hot_path.cmake
target_include_directories(lib_cobs PUBLIC .)
target_link_libraries(lib_cobs PUBLIC lib_types)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
