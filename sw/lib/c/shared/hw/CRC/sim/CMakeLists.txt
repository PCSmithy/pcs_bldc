add_library(hw_CRC STATIC HW_CRC.c)
target_include_directories(hw_CRC PUBLIC . ..)
target_link_libraries(hw_CRC
  PUBLIC
    lib_types
    lib_crc32
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
