# Channelized three-phase PWM driver (target-independent — actuates the bridge
# only through the HW_TIM output-compare + master-output-enable API, so it needs
# no per-target split).
add_library(io_PWM STATIC IO_PWM.c)
target_include_directories(io_PWM PUBLIC .)
target_link_libraries(io_PWM
  PUBLIC
    lib_types               # fixed-width types + float32_t + size_t
    hw_TIM                  # HW_TIM.h: channel enum, setCompare, MOE, getPeriod
    pcs_PWM_channels        # consumer-provided IO_PWM_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
