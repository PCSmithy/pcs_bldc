# pcs_ADC_channels: project-provided HW_ADC_channels.h extension point
# (channel enum, currently). hw_ADC's headers #include it.
add_library(pcs_ADC_channels INTERFACE)
target_include_directories(pcs_ADC_channels INTERFACE .)

# Project channel-config instance compiled into fw_hw, with deps on
# hw_ADC (struct definitions), lib_build (BUILD_TARGET_* constants),
# and lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_ADC_channels.c)
target_link_libraries(fw_hw PUBLIC hw_ADC lib_build lib_utils)
