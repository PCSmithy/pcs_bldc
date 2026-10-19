add_library(hw_CORDIC STATIC HW_CORDIC.c)
target_include_directories(hw_CORDIC PUBLIC . ..)
target_link_libraries(hw_CORDIC
  PUBLIC
    lib_types
    stm32g4_hal        # CORDIC register definitions + clock enable
)
