# IO-layer channelized library modules. Each module mirrors the hw/ pattern:
# a target-independent driver here, plus a project-provided pcs_*_channels
# include seam under sw/fw/src/io/<module>/. Only modules with a CMakeLists
# are wired up; the other IO_* directories are stubs awaiting their first use.
# io_AS5048 and io_SK6805 -> hw_SPI only; io_serial -> hw_USB. All build for
# either target and carry native Unity suites.
add_subdirectory(AS5048)
add_subdirectory(SK6805)
add_subdirectory(serial)
add_subdirectory(i2c)
add_subdirectory(PWM)
