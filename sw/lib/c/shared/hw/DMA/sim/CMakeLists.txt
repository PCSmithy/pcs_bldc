add_library(hw_DMA STATIC HW_DMA.c)
target_include_directories(hw_DMA PUBLIC . ..)
target_link_libraries(hw_DMA
  PUBLIC
    lib_types
    pcs_DMA_channels        # consumer-provided HW_DMA_channels.h path
    hw_SIL_irq              # pended completion dispatch (null-safe)
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
