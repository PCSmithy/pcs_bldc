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
# FreeRTOS kernel (vendor, third-party) -> sw/lib/c/FreeRTOS/
# Native-API subset: kernel sources + Cortex-M4F port + heap_4 + headers.
# The CMSIS-RTOS2 wrapper (CMSIS_RTOS_V2/) is intentionally NOT vendored —
# application code uses the native FreeRTOS API. The hand-written
# CMakeLists.txt at the package root is preserved. Present only when FreeRTOS
# is enabled in the .ioc, so the whole block is guarded.
# ---------------------------------------------------------------------------
FREERTOS_SRC="${SRC}/Middlewares/Third_Party/FreeRTOS/Source"
FREERTOS_DST="${LIB_C}/FreeRTOS"
if [ -d "${FREERTOS_SRC}" ]; then
  echo "==> Refreshing ${FREERTOS_DST}/ (vendor content only — CMakeLists.txt left alone)"
  # Wipe only the known vendor kernel sources (mirror of the cp list below).
  # Hand-authored root-level files (host_test_hooks.c) must survive the
  # round-trip, so no *.c glob here.
  rm -f  "${FREERTOS_DST}"/tasks.c "${FREERTOS_DST}"/queue.c \
         "${FREERTOS_DST}"/list.c "${FREERTOS_DST}"/timers.c \
         "${FREERTOS_DST}"/event_groups.c "${FREERTOS_DST}"/stream_buffer.c \
         "${FREERTOS_DST}"/croutine.c
  rm -rf "${FREERTOS_DST}/include"
  # Refresh only the vendor-managed portable subdirs. portable/Native-Fiber/ is
  # our hand-authored cooperative SIL port (not in the CubeMX tree) and must
  # survive the round-trip, so don't wipe portable/ wholesale.
  rm -rf "${FREERTOS_DST}/portable/GCC"
  rm -rf "${FREERTOS_DST}/portable/MemMang"

  mkdir -p "${FREERTOS_DST}/portable/GCC/ARM_CM4F" "${FREERTOS_DST}/portable/MemMang"
  cp "${FREERTOS_SRC}"/tasks.c "${FREERTOS_SRC}"/queue.c "${FREERTOS_SRC}"/list.c \
     "${FREERTOS_SRC}"/timers.c "${FREERTOS_SRC}"/event_groups.c \
     "${FREERTOS_SRC}"/stream_buffer.c "${FREERTOS_SRC}"/croutine.c "${FREERTOS_DST}/"
  cp -r "${FREERTOS_SRC}/include" "${FREERTOS_DST}/"
  cp "${FREERTOS_SRC}/portable/GCC/ARM_CM4F/port.c" \
     "${FREERTOS_SRC}/portable/GCC/ARM_CM4F/portmacro.h" \
     "${FREERTOS_DST}/portable/GCC/ARM_CM4F/"
  cp "${FREERTOS_SRC}/portable/MemMang/heap_4.c" "${FREERTOS_DST}/portable/MemMang/"
fi

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
# An IDE/indexer can hold one of these open on Windows; a busy rm is only a
# warning (the cp below overwrites), while a failed cp still aborts loudly.
for f in main.h stm32g4xx_hal_conf.h stm32g4xx_hal_msp.c stm32g4xx_it.c \
         stm32g4xx_it.h STM32G431VBTX_FLASH.ld startup_stm32g431vbtx.s \
         FreeRTOSConfig.h stm32g4xx_hal_timebase_tim.c; do
  rm -f "${FW_HW}/${f}" \
    || echo "  WARN: ${f} is in use; overwriting in place" >&2
done

mkdir -p "${FW_HW}"
cp "${SRC}/Core/Src/stm32g4xx_hal_msp.c"          "${FW_HW}/"
cp "${SRC}/Core/Src/stm32g4xx_it.c"               "${FW_HW}/"
cp "${SRC}/Core/Inc/main.h"                       "${FW_HW}/"
cp "${SRC}/Core/Inc/stm32g4xx_hal_conf.h"         "${FW_HW}/"
cp "${SRC}/Core/Inc/stm32g4xx_it.h"               "${FW_HW}/"
cp "${SRC}/Core/Startup/startup_stm32g431vbtx.s"  "${FW_HW}/"
cp "${SRC}/STM32G431VBTX_FLASH.ld"                "${FW_HW}/"

# FreeRTOS board config + HAL TIM6 timebase. Present only when FreeRTOS is
# enabled in the .ioc (which moves the HAL timebase off SysTick onto TIM6), so
# guard the copies. FreeRTOSConfig.h carries a USER CODE edit
# (xPortSysTickHandler -> SysTick_Handler) that CubeMX preserves in the
# reference tree.
if [ -f "${SRC}/Core/Inc/FreeRTOSConfig.h" ]; then
  cp "${SRC}/Core/Inc/FreeRTOSConfig.h"             "${FW_HW}/"
fi
if [ -f "${SRC}/Core/Src/stm32g4xx_hal_timebase_tim.c" ]; then
  cp "${SRC}/Core/Src/stm32g4xx_hal_timebase_tim.c" "${FW_HW}/"
fi

# Note: the CubeMX-generated main.c is intentionally NOT copied — it would
# add visual noise next to our real main.c at sw/fw/src/main.c. Read it
# in the reference tree at sw/fw/stm32cube/g4/Core/Src/main.c when you
# need to see SystemClock_Config / MX_*_Init bodies.

# ---------------------------------------------------------------------------
# Channelized HW_*_channels.cubemx.h headers — auto-generated macros
# extracted from CubeMX main.c's MX_GPIO_Init / MX_ADC*_Init / MX_SPI*_Init
# function bodies. Keeps the CubeMX-derived field values for
# HW_GPIO_channels.c, HW_ADC_channels.c and HW_SPI_channels.c in sync with
# the .ioc without hand-porting.
# ---------------------------------------------------------------------------
echo
echo "==> Regenerating HW_*_channels.cubemx.h from CubeMX main.c"
PYTHON=$(command -v python3 || command -v python || true)
if [ -z "${PYTHON}" ]; then
  echo "  WARN: python3/python not on PATH; skipping channels.cubemx.h generation." >&2
  echo "        Run tools/cubemx_to_channels.py manually after installing Python 3.10+." >&2
else
  "${PYTHON}" "${REPO_ROOT}/tools/cubemx_to_channels.py"
fi

echo
echo "==> Done."
echo
echo "If SystemClock_Config changed in CubeMX, diff"
echo "  ${SRC}/Core/Src/main.c"
echo "against HW_systemClock_init / clock_init() and hand-merge."
echo "(GPIO / ADC / SPI channel configs auto-update via the .cubemx.h headers.)"

# ---------------------------------------------------------------------------
# CubeMX rewrites every file it touches, flipping line endings; git then
# lists dozens of "modified" files whose content is unchanged. For each such
# file (empty diff under --ignore-cr-at-eol), git add refreshes the index
# entry with the identical blob: the file drops out of git status and
# nothing lands staged.
# ---------------------------------------------------------------------------
echo "==> Clearing line-ending-only churn from git status"
# git status lists the churned files; git diff shows them empty (the
# line-ending limbo), so enumerate from status and add only the diff-empty
# ones — the add re-hashes to the identical blob, so nothing lands staged.
# That identical-blob premise needs core.autocrlf=true; without it (typical
# macOS clone) the add would stage real CRLF bytes, so skip instead.
if [ "$(git -C "${REPO_ROOT}" config --get core.autocrlf)" != "true" ]; then
  echo "  core.autocrlf != true; skipping (churn would stage as real changes)"
else
git -C "${REPO_ROOT}" status --porcelain -- \
    sw/fw/stm32cube sw/lib/c/CMSIS sw/lib/c/STM32G4xx_HAL_Driver \
    sw/lib/c/FreeRTOS sw/lib/c/shared/hw/stm32g4 sw/fw/src/hw \
  | sed -n 's/^ M //p' \
  | while read -r f; do
      if git -C "${REPO_ROOT}" diff --ignore-cr-at-eol --quiet -- "$f"; then
        git -C "${REPO_ROOT}" add -- "$f"
      fi
    done
fi
echo "==> Done"
