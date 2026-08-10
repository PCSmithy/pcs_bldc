add_library(hw_SPI STATIC HW_SPI.c)
target_include_directories(hw_SPI PUBLIC .)
target_link_libraries(hw_SPI
  PUBLIC
    lib_types
    pcs_SPI_channels        # consumer-provided HW_SPI_channels.h path
    hw_GPIO                 # chip-select drive + SIL pin inspection
    hw_SPI_common           # target-independent timeout formula
    hw_SIL_ports            # duplex transaction seam (null-safe when no hooks installed)
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
