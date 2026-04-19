# Native (host) toolchain — used for SIL builds and unit tests.
#
# Goals:
#   - Use the host gcc (from MinGW on Windows, system gcc on macOS).
#   - Stay in the GCC family for parity with the embedded toolchain
#     (matches diagnostics, language semantics, sanitizer behavior).
#   - Be intentionally simple: no sanitizers in the default build (they're
#     useful but flaky on some MinGW setups). Add via -DPCS_SANITIZE=ON
#     once we know we want them.

set(PCS_TARGET "native" CACHE STRING "Build target identifier")

# Find host GCC explicitly so CMake doesn't auto-pick MSVC on Windows when
# invoked outside a Visual Studio dev shell.
find_program(NATIVE_C   gcc REQUIRED)
find_program(NATIVE_CXX g++ REQUIRED)

set(CMAKE_C_COMPILER   "${NATIVE_C}")
set(CMAKE_CXX_COMPILER "${NATIVE_CXX}")

add_compile_options(
  -Wall -Wextra -Wpedantic
  -g -O0
)
