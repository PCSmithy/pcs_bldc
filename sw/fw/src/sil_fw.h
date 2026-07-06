#pragma once

/*
 * SIL control ABI (D2) — the only hand-written Rust<->C surface.
 *
 * The framework drives the firmware through these three calls. All firmware
 * *data* flows by reading/writing firmware memory directly (the State Table),
 * never via sim-specific getters/setters. See docs/sil/ffi-boundary.md and
 * docs/sil/freertos-tick.md.
 *
 * This is "control, not data": you cannot advance the scheduler by poking a
 * variable, so these stay functions. Pacing (realtime vs fast) is the *driver's*
 * choice — whoever calls sil_fw_advance_tick() decides whether to pace to
 * wall-clock or run flat out; the firmware exposes only the per-tick primitive.
 */

#include "lib_types.h"
#include "SIL_ports.h"

/* Install the port-registration hook vtable (see SIL_ports.h): the seam sim
 * HW drivers use to expose runtime-registered signals ("ports") in native
 * units. Called by the framework BEFORE sil_fw_start so drivers can register
 * ports during init; NULL uninstalls. The struct is copied. With no hooks
 * installed the drivers behave exactly as standalone (SIL_ports is
 * null-safe). */
void sil_fw_setHooks(const SIL_ports_hooks_S * const hooks);

/* HW init + create tasks + run the scheduler to first quiescence. Returns
 * false on init / task-creation failure — the framework reports it; the
 * firmware never calls Error_Handler in SIL. */
bool sil_fw_start(void);

/* Advance one sim tick: run the firmware to the next quiescence (D1) and
 * return. The framework calls this once per base dt. */
void sil_fw_advance_tick(void);

/* Tear down the scheduler. */
void sil_fw_shutdown(void);

