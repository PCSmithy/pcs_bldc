add_library(hw_OPAMP STATIC HW_OPAMP.c)
target_include_directories(hw_OPAMP PUBLIC .)
target_link_libraries(hw_OPAMP
  PUBLIC
    lib_types
    pcs_OPAMP_channels   # consumer-provided HW_OPAMP_channels.h path
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
