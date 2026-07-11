
add_library(pcs_motorControl_channels INTERFACE)
target_include_directories(pcs_motorControl_channels INTERFACE .)

add_library(app_motorControl_channels STATIC app_motorControl_channels.c)
target_include_directories(app_motorControl_channels PUBLIC .)
target_link_libraries(app_motorControl_channels
    PUBLIC
        lib_utils
        dev_gateDriver
        app_motorControl
)
