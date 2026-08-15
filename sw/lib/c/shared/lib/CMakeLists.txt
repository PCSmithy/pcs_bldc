# Cross-cutting libraries (anyone in any layer may depend on these).

add_subdirectory(build)
add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(ringbuf)
add_subdirectory(cobs)
add_subdirectory(crc32)
add_subdirectory(protobuf)
add_subdirectory(timer)
add_subdirectory(CYPD3177)
# Future modules:
# add_subdirectory(nvm)
# add_subdirectory(rtos)
