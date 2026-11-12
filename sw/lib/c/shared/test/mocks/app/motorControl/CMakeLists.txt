# Reusable app_motorControl boundary double: the snapshot a test sets, and a
# recorder behind every setter. Consumed by app_sequencer and the board's
# server hooks (app_server_config).
add_library(mock_app_motorControl STATIC mock_app_motorControl.c)
target_include_directories(mock_app_motorControl PUBLIC .)
target_link_libraries(mock_app_motorControl PUBLIC lib_types)
