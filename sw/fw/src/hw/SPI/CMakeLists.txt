# pcs_SPI_channels: project-provided HW_SPI_channels.h extension point
# (channels enum). hw_SPI's headers #include it.
add_library(pcs_SPI_channels INTERFACE)
target_include_directories(pcs_SPI_channels INTERFACE .)

# Project channel-config instance compiled into fw_hw, with deps on
# hw_SPI (struct definitions), lib_build (BUILD_TARGET_* constants),
# and lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_SPI_channels.c)
target_link_libraries(fw_hw PUBLIC hw_SPI lib_build lib_utils)
