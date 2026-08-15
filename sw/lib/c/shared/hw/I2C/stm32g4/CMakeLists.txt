add_library(hw_I2C STATIC HW_I2C.c)
target_include_directories(hw_I2C PUBLIC . ..)
target_link_libraries(hw_I2C
  PUBLIC
    lib_types
    stm32g4_hal             # for I2C_HandleTypeDef + HAL_I2C_* prototypes
    pcs_I2C_channels        # consumer-provided HW_I2C_channels.h path
    freertos                # per-bus completion semaphore + task suspend
    hw_I2C_common           # target-independent timeout formula
)
