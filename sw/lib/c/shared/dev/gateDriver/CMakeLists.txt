# STSPIN32G4 gate-driver runtime driver (target-independent — programs and
# polls the gate-driver die's registers through the IO_i2c device API, so it
# carries no per-target split).
add_library(dev_gateDriver STATIC dev_gateDriver.c)
target_include_directories(dev_gateDriver PUBLIC .)
target_link_libraries(dev_gateDriver
  PUBLIC
    lib_types
    io_i2c                    # IO_i2c_readReg/writeReg + device enum
    pcs_gateDriver_channels   # consumer-provided dev_gateDriver_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
