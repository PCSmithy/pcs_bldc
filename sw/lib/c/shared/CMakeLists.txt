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
endif()
# add_subdirectory(app)
# add_subdirectory(dev)
# add_subdirectory(io)
