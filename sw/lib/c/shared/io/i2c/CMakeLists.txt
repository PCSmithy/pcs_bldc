# Channelized I2C device driver (target-independent — talks to devices only
# through the HW_I2C register-transfer API, so it needs no per-target split).
add_library(io_i2c STATIC IO_i2c.c)
target_include_directories(io_i2c PUBLIC .)
target_link_libraries(io_i2c
  PUBLIC
    lib_types               # fixed-width types + size_t
    hw_I2C                  # HW_I2C.h: bus enum, memAddrSize, memRead/memWrite
    pcs_i2c_channels        # consumer-provided IO_i2c_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
