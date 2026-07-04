add_library(hw_I2C STATIC HW_I2C.c)
target_include_directories(hw_I2C PUBLIC .)
target_link_libraries(hw_I2C
  PUBLIC
    lib_types
    pcs_I2C_channels        # consumer-provided HW_I2C_channels.h path
    hw_I2C_common           # target-independent timeout formula
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
