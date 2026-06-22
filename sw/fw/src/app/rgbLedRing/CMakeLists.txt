# Include-path-only seam: exposes the project app_rgbLedRing_channels.h (the
# channel enum) to the shared app_rgbLedRing module. Mirrors pcs_switch_channels.
# It must carry NO link deps — app_rgbLedRing links it for the header, and this
# would otherwise close a cycle (app_rgbLedRing_channels -> app_rgbLedRing -> here).
add_library(pcs_rgbLedRing_channels INTERFACE)
target_include_directories(pcs_rgbLedRing_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared module
# (app_rgbLedRing.h) and, through it, the io/dev channel enums it maps.
add_library(app_rgbLedRing_channels STATIC app_rgbLedRing_channels.c)
target_include_directories(app_rgbLedRing_channels PUBLIC .)
target_link_libraries(app_rgbLedRing_channels PUBLIC app_rgbLedRing lib_utils)
