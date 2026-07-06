# pcs_PWM_channels: include-path-only seam exposing the project
# IO_PWM_channels.h (phase enum) to the shared io_PWM driver. Carries NO link
# deps — io_PWM links it for the header, and a dep here would close a cycle
# (io_PWM_channels -> io_PWM -> here). Distinct from the hw-layer
# pcs_TIM_channels; do not reuse it.
add_library(pcs_PWM_channels INTERFACE)
target_include_directories(pcs_PWM_channels INTERFACE .)

# Project phase-config translation unit. Pulls in the shared driver (IO_PWM.h)
# and HW_TIM (HW_TIM_channels_E).
add_library(io_PWM_channels STATIC IO_PWM_channels.c)
target_include_directories(io_PWM_channels PUBLIC .)
target_link_libraries(io_PWM_channels PUBLIC io_PWM hw_TIM lib_utils)
