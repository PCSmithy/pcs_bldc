
add_library(pcs_rotorEstimation_channels INTERFACE)
target_include_directories(pcs_rotorEstimation_channels INTERFACE .)

add_library(app_rotorEstimation_channels STATIC app_rotorEstimation_channels.c)
target_include_directories(app_rotorEstimation_channels PUBLIC .)
target_link_libraries(app_rotorEstimation_channels
    PUBLIC
        lib_utils
        app_rotorEstimation
)
