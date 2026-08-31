add_library(lib_crc32 STATIC
  lib_crc32.c
)
target_include_directories(lib_crc32 PUBLIC .)
target_link_libraries(lib_crc32 PUBLIC lib_types)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
