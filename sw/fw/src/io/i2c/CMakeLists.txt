# pcs_i2c_channels: include-path-only seam exposing the project
# IO_i2c_channels.h (device enum) to the shared io_i2c driver. Carries NO link
# deps — io_i2c links it for the header, and a dep here would close a cycle
# (io_i2c_channels -> io_i2c -> here). Distinct from the hw-layer
# pcs_I2C_channels; do not reuse it.
add_library(pcs_i2c_channels INTERFACE)
target_include_directories(pcs_i2c_channels INTERFACE .)

# Project device-config translation unit. Pulls in the shared driver
# (IO_i2c.h) and HW_I2C (HW_I2C_bus_E + memAddrSize).
add_library(io_i2c_channels STATIC IO_i2c_channels.c)
target_include_directories(io_i2c_channels PUBLIC .)
target_link_libraries(io_i2c_channels PUBLIC io_i2c hw_I2C lib_utils)
