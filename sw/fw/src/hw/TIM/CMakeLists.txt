# pcs_TIM_channels: project-provided HW_TIM_channels.h extension point
# (channel enum, currently). hw_TIM's headers #include it.
add_library(pcs_TIM_channels INTERFACE)
target_include_directories(pcs_TIM_channels INTERFACE .)

# Project channel-config instance compiled into fw_hw, with deps on
# hw_TIM (struct definitions), lib_build (BUILD_TARGET_* constants),
# and lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_TIM_channels.c)
target_link_libraries(fw_hw PUBLIC hw_TIM lib_build lib_utils)
