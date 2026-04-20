# Native (SIL) fw_hw — STATIC for symmetry with the embedded side, so
# per-module project channel configs (HW_*_config.c) can attach via
# target_sources(fw_hw PRIVATE ...) without needing the INTERFACE
# keyword juggling that an INTERFACE fw_hw would force.
#
# _placeholder.c exists only to give the STATIC library a source file
# (CMake errors on a STATIC library with no sources). When real sim
# infrastructure lands here, delete _placeholder.c.

add_library(fw_hw STATIC _placeholder.c)
