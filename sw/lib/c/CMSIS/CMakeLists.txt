# ARM CMSIS — Cortex-M software interface standard.
#
# Header-only — defined as an INTERFACE library so consumers just
# target_link_libraries(... PRIVATE cmsis) and pick up the include
# paths.
#
# Bundle ships with both vendor-agnostic Cortex-M Core headers (in
# Include/) and ST's STM32G4 device headers (in Device/ST/STM32G4xx/),
# because we got this CMSIS copy from ST's CubeMX output. If we ever
# add a non-ST MCU, we'd reorganize to split per-vendor device headers.
#
# Vendor content (Include/, Device/, LICENSE.txt) is auto-managed by
# tools/convert_cubemx_to_canonical.sh; this CMakeLists.txt is
# hand-written and the script preserves it.

add_library(cmsis INTERFACE)
target_include_directories(cmsis INTERFACE
  Include
  Device/ST/STM32G4xx/Include
)
