# Channelized SPI library.
#
# Both target subdirs define a library named `hw_SPI`; only one is
# added per build, selected by PCS_TARGET.

# Target-independent helpers shared by both impls (and unit-testable on
# their own). The timeout formula lives here so native tests can exercise
# it directly without the stm32g4 clock machinery.
add_library(hw_SPI_common STATIC HW_SPI_timeout.c)
target_include_directories(hw_SPI_common PUBLIC .)
target_link_libraries(hw_SPI_common PUBLIC lib_types)

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
elseif(PCS_TARGET STREQUAL "native")
  add_subdirectory(sim)
else()
  message(FATAL_ERROR
    "Unknown PCS_TARGET '${PCS_TARGET}'; expected 'embedded' or 'native'")
endif()
