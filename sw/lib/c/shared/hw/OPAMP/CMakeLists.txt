# Channelized OPAMP library.
#
# Both target subdirs define a library named `hw_OPAMP`; only one is
# added per build, selected by PCS_TARGET.

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
elseif(PCS_TARGET STREQUAL "native")
  add_subdirectory(sim)
else()
  message(FATAL_ERROR
    "Unknown PCS_TARGET '${PCS_TARGET}'; expected 'embedded' or 'native'")
endif()
