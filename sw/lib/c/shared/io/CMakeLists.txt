# IO-layer channelized library modules. Each module mirrors the hw/ pattern:
# a target-independent driver here, plus a project-provided pcs_*_channels
# include seam under sw/fw/src/io/<module>/. Only modules with a CMakeLists
# are wired up; the other IO_* directories are stubs awaiting their first use.
# io_AS5048 -> hw_SPI only, so it builds for both targets (and carries a
# native Unity suite). SK6805 stays embedded-only until it gets the same
# dual-target + test treatment.
add_subdirectory(AS5048)
if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(SK6805)
endif()
