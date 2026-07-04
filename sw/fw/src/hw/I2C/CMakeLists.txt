# pcs_I2C_channels: project-provided HW_I2C_channels.h extension point
# (bus enum). hw_I2C's headers #include it.
add_library(pcs_I2C_channels INTERFACE)
target_include_directories(pcs_I2C_channels INTERFACE .)

# Project bus-config instance compiled into fw_hw, with deps on hw_I2C
# (struct definitions), lib_build (BUILD_TARGET_* constants), and
# lib_utils (COUNTOF macro).
target_sources(fw_hw PRIVATE HW_I2C_channels.c)
target_link_libraries(fw_hw PUBLIC hw_I2C lib_build lib_utils)
