add_library(hw_GPIO STATIC HW_GPIO.c)
target_include_directories(hw_GPIO PUBLIC .)
target_link_libraries(hw_GPIO
  PUBLIC
    lib_types
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
