# pcs_OPAMP_channels: project-provided HW_OPAMP_channels.h extension point
# (channel enum, currently). hw_OPAMP's headers #include it.
add_library(pcs_OPAMP_channels INTERFACE)
target_include_directories(pcs_OPAMP_channels INTERFACE .)

# Project channel-config instance compiled into fw_hw, with deps on
# hw_OPAMP (struct definitions), lib_build (BUILD_TARGET_* constants),
# and lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_OPAMP_channels.c)
target_link_libraries(fw_hw PUBLIC hw_OPAMP lib_build lib_utils)
