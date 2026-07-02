# pcs_DMA_channels: project-provided HW_DMA_channels.h extension point
# (channels enum). hw_DMA's headers #include it.
add_library(pcs_DMA_channels INTERFACE)
target_include_directories(pcs_DMA_channels INTERFACE .)

# Project channel-config instance compiled into fw_hw, with deps on
# hw_DMA (struct definitions), lib_build (BUILD_TARGET_* constants),
# and lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_DMA_channels.c)
target_link_libraries(fw_hw PUBLIC hw_DMA lib_build lib_utils)
