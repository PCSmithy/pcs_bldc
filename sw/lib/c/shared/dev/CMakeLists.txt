# dev-layer modules: device logic layered on the hw drivers (debounced
# switches, observers, ...). Add a module's subdir once it has real code.
add_subdirectory(switch)
add_subdirectory(CYPD3177)
