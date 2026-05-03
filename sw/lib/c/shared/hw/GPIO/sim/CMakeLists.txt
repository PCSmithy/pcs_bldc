add_library(hw_GPIO STATIC HW_GPIO.c)
target_include_directories(hw_GPIO PUBLIC .)
target_link_libraries(hw_GPIO
  PUBLIC
    lib_types
)
