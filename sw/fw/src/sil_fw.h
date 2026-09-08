#pragma once

/*
 * SIL control ABI — the only hand-written Rust<->C surface.
 *
 * The framework drives the firmware through these three calls. All firmware
 * *data* flows by reading/writing firmware memory directly (the State Table),
 * never via sim-specific getters/setters. See docs/sil/ffi-boundary.md and
 * docs/sil/freertos-tick.md.
 *
 * This is "control, not data": you cannot advance the scheduler by poking a
 * variable, so these stay functions. Pacing (realtime vs fast) is the *driver's*
 * choice — whoever steps the firmware decides whether to pace to wall-clock or
 * run flat out; the firmware exposes only the per-step primitives.
 */

#include "lib_types.h"
#include "SIL_ports.h"
#include "SIL_irq.h"

/* Install the port-registration hook vtable (see SIL_ports.h): the seam sim
 * HW drivers use to expose runtime-registered signals ("ports") in native
 * units. Called by the framework BEFORE sil_fw_start so drivers can register
 * ports during init; NULL uninstalls. The struct is copied. With no hooks
 * installed the drivers behave exactly as standalone (SIL_ports is
 * null-safe). */
void sil_fw_setHooks(const SIL_ports_hooks_S * const hooks);

/* Install the simulated-interrupt hook vtable (see SIL_irq.h): the seam sim HW
 * drivers use to register handlers with the framework's interrupt table.
 * A separate vtable from the port hooks above — a different seam, separately
 * versionable, and neither module needs the other's types. Same contract:
 * called BEFORE sil_fw_start, NULL uninstalls, the struct is copied, and with
 * no hooks installed the drivers behave exactly as standalone. */
void sil_fw_setIrqHooks(const SIL_irq_hooks_S * const hooks);

/* HW init + create tasks + run the scheduler to first quiescence. Returns
 * false on init / task-creation failure — the framework reports it; the
 * firmware never calls Error_Handler in SIL. */
bool sil_fw_start(void);

/* Advance the hardware timebase by elapsed_us. The framework calls this once per
 * base dt, BEFORE dispatching anything due that step, so a handler reads the
 * timebase of the step it runs in. Runs no firmware code of its own — the kernel
 * tick is an interrupt like any other (docs/sil/sim-interrupts.md). */
void sil_fw_advance_time(uint32_t elapsed_us);

/* Run one simulated interrupt handler in the firmware fiber, bracketed by the
 * port's ISR entry/exit so ...FromISR wakeups and portYIELD_FROM_ISR behave as
 * on hardware — a task the handler unblocks runs before this call returns.
 * Returns false when firmware interrupts are masked (critical section /
 * disabled): the handler did NOT run and the framework holds it pending. */
bool sil_fw_dispatch_isr(SIL_irq_handler_F handler);

/* Tear down the scheduler. */
void sil_fw_shutdown(void);

