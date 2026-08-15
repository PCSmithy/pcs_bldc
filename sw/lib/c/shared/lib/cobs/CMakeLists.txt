add_library(lib_cobs STATIC
  lib_cobs.c
)
target_include_directories(lib_cobs PUBLIC .)
target_link_libraries(lib_cobs PUBLIC lib_types)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
