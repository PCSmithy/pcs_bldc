add_library(hw_CORDIC STATIC HW_CORDIC.c)
target_include_directories(hw_CORDIC PUBLIC . ..)
target_link_libraries(hw_CORDIC
  PUBLIC
    lib_types
    m
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
