# Channelized I2C library.
#
# Both target subdirs define a library named `hw_I2C`; only one is
# added per build, selected by PCS_TARGET.

# Target-independent helpers shared by both impls (and unit-testable on
# their own). The timeout formula lives here so native tests can exercise
# it directly without the stm32g4 clock machinery.
add_library(hw_I2C_common STATIC HW_I2C_timeout.c)
target_include_directories(hw_I2C_common PUBLIC .)
target_link_libraries(hw_I2C_common PUBLIC lib_types)

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
elseif(PCS_TARGET STREQUAL "native")
  add_subdirectory(sim)
else()
  message(FATAL_ERROR
    "Unknown PCS_TARGET '${PCS_TARGET}'; expected 'embedded' or 'native'")
endif()
