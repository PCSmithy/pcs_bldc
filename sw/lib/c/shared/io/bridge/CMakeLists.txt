# Channelized three-phase motor-bridge driver (target-uniform — actuates the
# bridge through the HW_TIM output-compare + master-output-enable API and reads
# the phase/bus current shunts through HW_ADC, so it needs no per-target split).
add_library(io_bridge STATIC IO_bridge.c)
target_include_directories(io_bridge PUBLIC .)
target_link_libraries(io_bridge
  PUBLIC
    lib_types               # fixed-width types + float32_t + size_t
    hw_TIM                  # HW_TIM.h: channel/peripheral enums, setCompare, MOE, getPeriod
    hw_ADC                  # HW_ADC.h: channel enum + getVolts (current-sense readback)
    pcs_bridge_channels     # consumer-provided IO_bridge_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
