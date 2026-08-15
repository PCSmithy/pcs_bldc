add_library(hw_systemClock STATIC HW_systemClock.c)
target_include_directories(hw_systemClock PUBLIC . ..)
target_link_libraries(hw_systemClock
  PUBLIC
    lib_types
    pcs_systemClock_config  # consumer-provided HW_systemClock_config.h path
)
