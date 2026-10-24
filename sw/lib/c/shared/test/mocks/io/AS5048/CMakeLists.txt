# Reusable IO_AS5048 boundary double: a settable radian angle and integrity
# status per channel, behind the driver's readAngle/getStatus surface.
# Consumed by app_motorControl and app_rotorEstimation.
add_library(mock_io_AS5048 STATIC mock_IO_AS5048.c)
target_include_directories(mock_io_AS5048 PUBLIC .)
target_link_libraries(mock_io_AS5048 PUBLIC lib_types)
