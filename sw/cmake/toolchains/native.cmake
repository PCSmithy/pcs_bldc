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

# Preprocessor-visible build target. Source files use
#   #if (BUILD_TARGET == BUILD_TARGET_SIM) ...
# with the named constants defined in sw/lib/c/shared/lib/build/lib_build.h.
add_compile_definitions(BUILD_TARGET=BUILD_TARGET_SIM)

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

# The firmware links as a SHARED library (SIL). On Linux/ELF that requires all
# objects — including the static libs it pulls in — to be position-independent,
# or the .so link fails with "final link failed: bad value". Harmless on macOS
# (already PIC) and MinGW (ignored).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
