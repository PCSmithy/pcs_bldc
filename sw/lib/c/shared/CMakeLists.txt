# Layer roots for our own library code (mirroring the firmware project's
# sw/fw/src/ layout). Add a layer's subdirectory only once it has its
# first real module — no point processing empty layers.

add_subdirectory(lib)

# Shared unit-test mocks (reusable test doubles as CMake targets). Native only —
# they exist purely to be linked into Unity suites.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test/mocks)
endif()

# Channelized hw/ modules need consumer-provided pcs_*_config interface
# libs (defined in sw/fw/src/hw/<module>/CMakeLists.txt). Skip the whole
# hw/ tree when building lib/c standalone so unit tests for cross-cutting
# libs (ringbuf etc.) can still run on their own.
if(PCS_BUILDING_FW)
  add_subdirectory(hw)

  # io/ modules consume project-provided pcs_*_channels interface libs from
  # sw/fw/src/io/<module>/. Modules with only dual-target HW deps (io_AS5048
  # -> hw_SPI) build for both targets; the per-module gating lives in
  # io/CMakeLists.txt (only io/usb -> tinyusb stays embedded-only).
  add_subdirectory(io)

  # dev/ likewise consumes project-provided pcs_*_channels seams from
  # sw/fw/src/dev/. dev_switch is target-independent (only needs HW_GPIO), so
  # it builds for both targets and carries a native Unity suite for its
  # debounce logic.
  add_subdirectory(dev)

  # app/ modules (device UX) layer on the io + dev drivers. Target-independent;
  # the SIM executable doesn't link them yet, but their render cores carry
  # native Unity suites.
  add_subdirectory(app)
endif()
