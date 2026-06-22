# Debounced switch/button driver (target-independent — reads the cached GPIO
# input snapshot through HW_GPIO, applies a per-channel SW debounce).
add_library(dev_switch STATIC DEV_switch.c)
target_include_directories(dev_switch PUBLIC .)
target_link_libraries(dev_switch
  PUBLIC
    lib_types
    hw_GPIO                 # HW_GPIO_readCached + port/level types
    pcs_switch_channels     # consumer-provided DEV_switch_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
