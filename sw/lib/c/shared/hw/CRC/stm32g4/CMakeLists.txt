add_library(hw_CRC STATIC HW_CRC.c)
target_include_directories(hw_CRC PUBLIC . ..)
target_link_libraries(hw_CRC
  PUBLIC
    lib_types
    stm32g4_hal        # CRC register definitions + clock enable
)
