# Shared unit-test mocks (native only).
#
# Each subdirectory defines one reusable test double as a STATIC library that
# PUBLIC-exposes its include dir. A test links the mock target instead of the
# real hw/io/dev library: because these tests compile the module-under-test's
# .c directly and do NOT link the real dependency, `#include "HW_TIM.h"`
# resolves to the mock's shadow header and the mock .c supplies the symbols.
#
# Target naming mirrors the production libraries: mock_<layer>_<Module>
# (e.g. mock_hw_TIM, mock_hw_ADC). Each mock owns a canonical test channel
# enum in its shadow <Module>_channels.h; boundary tests build their config
# against those names.
#
# A mock lives here (rather than in one module's test/ dir) once more than one
# test suite needs it. Single-consumer doubles can stay local to their test/
# until a second consumer appears.

add_subdirectory(hw/TIM)
add_subdirectory(hw/ADC)
