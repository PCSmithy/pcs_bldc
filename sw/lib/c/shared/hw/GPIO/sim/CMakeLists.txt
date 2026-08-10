add_library(hw_GPIO STATIC HW_GPIO.c)
target_include_directories(hw_GPIO PUBLIC .)
target_link_libraries(hw_GPIO
  PUBLIC
    lib_types
    hw_SIL_ports       # output-pin observation ports (null-safe when no hooks installed)
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
