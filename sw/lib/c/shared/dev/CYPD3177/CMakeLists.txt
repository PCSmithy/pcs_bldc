# CYPD3177 USB-PD sink runtime driver (target-independent — fetches status
# registers through the IO_i2c device API and decodes them with lib_CYPD3177,
# so it carries no per-target split).
add_library(dev_CYPD3177 STATIC dev_CYPD3177.c)
target_include_directories(dev_CYPD3177 PUBLIC .)
target_link_libraries(dev_CYPD3177
  PUBLIC
    lib_types
    lib_CYPD3177             # register offsets + pure decoders
    io_i2c                   # IO_i2c_readReg + device enum
    pcs_CYPD3177_channels    # consumer-provided dev_CYPD3177_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
