# sw/lib/c — channelized library implementations (the "platform").
#
# Each library is a STATIC archive consumable by either the firmware
# project (sw/fw) or future Rust app (via FFI). All libraries here must
# build for both PCS_TARGET=native and PCS_TARGET=embedded.
#
# Library naming convention: <layer>_<module>
#   layer  ∈ {app, dev, io, hw, lib}
#   module is the module's directory name
#   e.g. sw/lib/c/lib/ringbuf/  ->  target name: lib_ringbuf

cmake_minimum_required(VERSION 3.20)
project(pcs_lib_c LANGUAGES C ASM)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# Tests are only meaningful (and runnable) on the native target. Unity
# (vendored at sw/lib/c/Unity) is only built when tests will be built.
if(PCS_TARGET STREQUAL "native")
  enable_testing()
  add_subdirectory(Unity)
endif()

# Layer roots. Add modules as they appear; do not pre-add empty layers.
add_subdirectory(lib)
# add_subdirectory(app)
# add_subdirectory(dev)
# add_subdirectory(io)
# add_subdirectory(hw)
