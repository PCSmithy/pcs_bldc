add_library(hw_systemClock STATIC HW_systemClock.c)
target_include_directories(hw_systemClock PUBLIC . ..)
target_link_libraries(hw_systemClock
  PUBLIC
    lib_types
    stm32g4_hal             # for RCC_*InitTypeDef + HAL_* prototypes
)
