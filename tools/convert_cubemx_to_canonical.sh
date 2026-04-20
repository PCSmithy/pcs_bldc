#!/usr/bin/env bash
# Copy STM32CubeMX-generated code from sw/fw/stm32cube/g4/ into the
# canonical project layout.
#
# Auto-generated files land flat in:
#   sw/lib/c/hw/stm32g4/   (vendor + generic STM32G4 family code)
#   sw/fw/src/hw/stm32g4/  (board-specific generated code)
#
# Hand-written files (CMakeLists.txt, board.c, board.h, etc.) live in
# the same directories alongside the generated ones; this script only
# touches a known fixed list of generated filenames and never anything
# else. CubeMX's filenames are stable across regenerations of a given
# .ioc, so the explicit list works.
#
# Convention: vendor files are named `stm32g4xx_*`, `startup_*`,
# `STM32G431*.ld`, `system_*`, `syscalls.c`, `sysmem.c`, or live in the
# CMSIS/ and STM32G4xx_HAL_Driver/ vendor directories. Anything not
# matching that is hand-written.
#
# The original sw/fw/stm32cube/g4/ tree is left untouched as reference.
# When CubeMX regenerates a new clock tree or peripheral list, diff
# sw/fw/stm32cube/g4/Core/Src/main.c against board.c and hand-merge.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${REPO_ROOT}/sw/fw/stm32cube/g4"
LIB_C="${REPO_ROOT}/sw/lib/c"
LIB_HW="${LIB_C}/shared/hw/stm32g4"
FW_HW="${REPO_ROOT}/sw/fw/src/hw/stm32g4"

[ -d "${SRC}" ] || { echo "Error: expected reference source tree at ${SRC}" >&2; exit 1; }

# ---------------------------------------------------------------------------
# CMSIS (ARM Cortex-M standard, vendor-agnostic) -> sw/lib/c/CMSIS/
# Lives at the top of sw/lib/c/ alongside other third-party packages.
# Includes Cortex-M Core headers (any Cortex-M MCU) plus ST's STM32G4
# device headers (Device/ST/STM32G4xx/) which travel with the bundle.
# ---------------------------------------------------------------------------
echo "==> Refreshing ${LIB_C}/CMSIS/ (vendor content only — CMakeLists.txt left alone)"
rm -rf "${LIB_C}/CMSIS/Include"
rm -rf "${LIB_C}/CMSIS/Device"
rm -f  "${LIB_C}/CMSIS/LICENSE.txt"

mkdir -p "${LIB_C}/CMSIS"
cp -r "${SRC}/Drivers/CMSIS/Include"     "${LIB_C}/CMSIS/"
cp -r "${SRC}/Drivers/CMSIS/Device"      "${LIB_C}/CMSIS/"
cp    "${SRC}/Drivers/CMSIS/LICENSE.txt" "${LIB_C}/CMSIS/"

# ---------------------------------------------------------------------------
# STM32G4xx HAL driver (vendor, family-specific) -> sw/lib/c/STM32G4xx_HAL_Driver/
# Top-level vendored package alongside CMSIS / Unity. The hand-written
# CMakeLists.txt at the package root is preserved across regenerations.
# ---------------------------------------------------------------------------
echo "==> Refreshing ${LIB_C}/STM32G4xx_HAL_Driver/ (vendor content only — CMakeLists.txt left alone)"
rm -rf "${LIB_C}/STM32G4xx_HAL_Driver/Inc"
rm -rf "${LIB_C}/STM32G4xx_HAL_Driver/Src"
rm -f  "${LIB_C}/STM32G4xx_HAL_Driver/LICENSE.txt"

mkdir -p "${LIB_C}/STM32G4xx_HAL_Driver"
cp -r "${SRC}/Drivers/STM32G4xx_HAL_Driver/Inc"         "${LIB_C}/STM32G4xx_HAL_Driver/"
cp -r "${SRC}/Drivers/STM32G4xx_HAL_Driver/Src"         "${LIB_C}/STM32G4xx_HAL_Driver/"
cp    "${SRC}/Drivers/STM32G4xx_HAL_Driver/LICENSE.txt" "${LIB_C}/STM32G4xx_HAL_Driver/"

# ---------------------------------------------------------------------------
# STM32G4-family support files (system init + newlib syscall stubs)
# -> sw/lib/c/hw/stm32g4/
# These sit in the layer dir because they're family-specific glue, not
# library code per se. The future channelized SPI/UART/etc. wrappers we
# write will live alongside them.
# ---------------------------------------------------------------------------
echo "==> Refreshing ${LIB_HW}/ (vendor support files only)"
rm -f "${LIB_HW}/system_stm32g4xx.c"
rm -f "${LIB_HW}/syscalls.c"
rm -f "${LIB_HW}/sysmem.c"

mkdir -p "${LIB_HW}"
cp "${SRC}/Core/Src/system_stm32g4xx.c" "${LIB_HW}/"
cp "${SRC}/Core/Src/syscalls.c"         "${LIB_HW}/"
cp "${SRC}/Core/Src/sysmem.c"           "${LIB_HW}/"

# ---------------------------------------------------------------------------
# Board-specific generated code -> sw/fw/src/hw/stm32g4/
# ---------------------------------------------------------------------------
echo "==> Refreshing ${FW_HW}/ (vendor files only — board.c, CMakeLists.txt left alone)"
rm -f "${FW_HW}/main.h"
rm -f "${FW_HW}/stm32g4xx_hal_conf.h"
rm -f "${FW_HW}/stm32g4xx_hal_msp.c"
rm -f "${FW_HW}/stm32g4xx_it.c"
rm -f "${FW_HW}/stm32g4xx_it.h"
rm -f "${FW_HW}/STM32G431VBTX_FLASH.ld"
rm -f "${FW_HW}/startup_stm32g431vbtx.s"

mkdir -p "${FW_HW}"
cp "${SRC}/Core/Src/stm32g4xx_hal_msp.c"          "${FW_HW}/"
cp "${SRC}/Core/Src/stm32g4xx_it.c"               "${FW_HW}/"
cp "${SRC}/Core/Inc/main.h"                       "${FW_HW}/"
cp "${SRC}/Core/Inc/stm32g4xx_hal_conf.h"         "${FW_HW}/"
cp "${SRC}/Core/Inc/stm32g4xx_it.h"               "${FW_HW}/"
cp "${SRC}/Core/Startup/startup_stm32g431vbtx.s"  "${FW_HW}/"
cp "${SRC}/STM32G431VBTX_FLASH.ld"                "${FW_HW}/"

# Note: the CubeMX-generated main.c is intentionally NOT copied — it would
# add visual noise next to our real main.c at sw/fw/src/main.c. Read it
# in the reference tree at sw/fw/stm32cube/g4/Core/Src/main.c when you
# need to see SystemClock_Config / MX_*_Init bodies.

echo
echo "==> Done."
echo
echo "If the clock tree or peripheral list changed in CubeMX, diff"
echo "  ${SRC}/Core/Src/main.c"
echo "against the SystemClock_Config / clock_init() in"
echo "  ${FW_HW}/board.c"
echo "and hand-merge."
