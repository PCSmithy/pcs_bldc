add_library(pcs_sequencer_channels INTERFACE)
target_include_directories(pcs_sequencer_channels INTERFACE .)

add_library(app_sequencer_channels STATIC app_sequencer_channels.c)
target_include_directories(app_sequencer_channels PUBLIC .)
target_link_libraries(app_sequencer_channels
    PUBLIC
        lib_utils
        app_sequencer
)
