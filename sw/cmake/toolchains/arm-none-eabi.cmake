# Embedded toolchain — arm-none-eabi-gcc cross-compile for STM32G431 (Cortex-M4F).
#
# Goals:
#   - Find the Arm GNU Toolchain on PATH first; fall back to the standard
#     installer location on Windows / macOS so a fresh clone works.
#   - Set MCU flags globally (Cortex-M4F with single-precision FPU, hard ABI).
#   - Apply common code-quality flags consistent with the native toolchain.
#   - No newlib / libc decisions baked in here — those belong to the
#     executable target in fw/, not the toolchain (libraries don't link).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(PCS_TARGET             "embedded" CACHE STRING "Build target identifier")

# Preprocessor-visible build target. Source files use
#   #if (BUILD_TARGET == BUILD_TARGET_STM32G4) ...
# with the named constants defined in sw/lib/c/shared/lib/build/lib_build.h.
add_compile_definitions(BUILD_TARGET=BUILD_TARGET_STM32G4)

# Locate arm-none-eabi-gcc: PATH first, then known install locations.
find_program(ARM_GCC arm-none-eabi-gcc
  PATHS
    "C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin"
    "/Applications/ArmGNUToolchain/bin"
)
if(NOT ARM_GCC)
  message(FATAL_ERROR
    "arm-none-eabi-gcc not found on PATH or in standard install locations.\n"
    "Install the Arm GNU Toolchain from:\n"
    "  https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
  )
endif()

# Derive the executable extension from the found gcc path itself
# (CMAKE_EXECUTABLE_SUFFIX isn't populated until after project()).
get_filename_component(_arm_bin "${ARM_GCC}" DIRECTORY)
get_filename_component(_arm_ext "${ARM_GCC}" EXT)

set(CMAKE_C_COMPILER   "${_arm_bin}/arm-none-eabi-gcc${_arm_ext}")
set(CMAKE_CXX_COMPILER "${_arm_bin}/arm-none-eabi-g++${_arm_ext}")
set(CMAKE_ASM_COMPILER "${_arm_bin}/arm-none-eabi-gcc${_arm_ext}")
set(CMAKE_OBJCOPY      "${_arm_bin}/arm-none-eabi-objcopy${_arm_ext}" CACHE FILEPATH "")
set(CMAKE_SIZE         "${_arm_bin}/arm-none-eabi-size${_arm_ext}"    CACHE FILEPATH "")

# Cross-compiling: don't try to compile-and-run test programs during configure.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M4F (STM32G431): hard FPU ABI, single-precision, Thumb.
set(_mcu_flags
  -mcpu=cortex-m4
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -mthumb
)

# Optimization level. -Og optimizes for the debugging experience: it keeps the
# flashed image roughly 40% smaller than -O0 while preserving sane single-
# stepping and live variables, so it is the default. Override at configure time
# without editing this file:
#   -DPCS_OPT_LEVEL=-O0   source-faithful deep debugging (largest)
#   -DPCS_OPT_LEVEL=-Os   minimum size (poor debuggability)
set(PCS_OPT_LEVEL "-Og" CACHE STRING "Optimization level for the embedded build")

add_compile_options(
  ${_mcu_flags}
  -Wall -Wextra -Wpedantic
  # The FPU is single-precision: an implicit float->double promotion means
  # libgcc soft-double (~1.2 KB flash) plus slow soft-float math. Promote on
  # purpose (explicit cast) or not at all.
  -Wdouble-promotion
  -ffunction-sections -fdata-sections
  -fno-common
  ${PCS_OPT_LEVEL}
  # Full debug info for live (OpenOCD/GDB) debugging. It lands in the .elf only,
  # not in flashed sections, so it never costs flash regardless of opt level.
  -g3
)
add_link_options(
  ${_mcu_flags}
  -Wl,--gc-sections
)
