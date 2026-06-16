# Layer roots for our own library code (mirroring the firmware project's
# sw/fw/src/ layout). Add a layer's subdirectory only once it has its
# first real module — no point processing empty layers.

add_subdirectory(lib)

# Channelized hw/ modules need consumer-provided pcs_*_config interface
# libs (defined in sw/fw/src/hw/<module>/CMakeLists.txt). Skip the whole
# hw/ tree when building lib/c standalone so unit tests for cross-cutting
# libs (ringbuf etc.) can still run on their own.
if(PCS_BUILDING_FW)
  add_subdirectory(hw)

  # io/ modules consume project-provided pcs_*_channels interface libs from
  # sw/fw/src/io/<module>/. That tree (io/usb -> tinyusb) is embedded-only,
  # so the project channel configs only exist for the embedded target — gate
  # the shared io/ tree to match, otherwise a native build references an
  # undefined pcs_*_channels target.
  # dev/ likewise consumes project-provided pcs_*_channels seams from
  # sw/fw/src/dev/. dev_switch itself is target-independent (only needs
  # HW_GPIO), but its project config tree is currently wired embedded-only, so
  # gate the shared dev/ tree to match. Promote both to dual-target when adding
  # SIL unit tests for the debounce logic.
  if(PCS_TARGET STREQUAL "embedded")
    add_subdirectory(io)
    add_subdirectory(dev)
  endif()
endif()
# add_subdirectory(app)
