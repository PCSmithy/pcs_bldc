# IO-layer channelized library modules. Each module mirrors the hw/ pattern:
# a target-independent driver here, plus a project-provided pcs_*_channels
# include seam under sw/fw/src/io/<module>/. Only modules with a CMakeLists
# are wired up; the other IO_* directories are stubs awaiting their first use.
add_subdirectory(AS5048)
add_subdirectory(SK6805)
