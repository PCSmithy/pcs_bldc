#ifndef SIL_IRQ_H
#define SIL_IRQ_H

/*
 * SIL simulated-interrupt registration seam — native sim target only
 * (design: docs/sil/sim-interrupts.md).
 *
 * Sim HW drivers register handlers here; the framework schedules them on the
 * sim grid and dispatches each in the firmware fiber inside the port's ISR
 * bracket, so ...FromISR wakeups behave as on hardware. Periods/delays are
 * plain sim-microsecond literals — the seam does no clock arithmetic.
 *
 * Only sim-target HW-layer code (hw/<X>/sim/, sim port glue) may register;
 * portable app/io/dev firmware stays sim-unaware. The framework installs the
 * hooks via sil_fw_setIrqHooks BEFORE sil_fw_start; with none installed
 * (standalone runs, Unity) register returns SIL_IRQ_HANDLE_INVALID and the
 * rest are no-ops.
 */

#include "lib_types.h"

#define SIL_IRQ_HANDLE_INVALID  (-1)

// A simulated interrupt handler: an ordinary C function the framework calls in
// the firmware context. Not restricted to CMSIS vector names — a vector handler,
// a HAL callback, or a sim function all qualify.
typedef void (*SIL_irq_handler_F)(void);

typedef struct
{
    // Opaque framework context, passed back on every call.
    void * context;

    // Register a handler firing every period_us of sim time. Returns a handle,
    // or a negative value on failure. priority orders same-step dispatch only
    // (lower value first, no preemption); the first firing is one period away.
    int32_t (*registerPeriodic)(void * context, SIL_irq_handler_F handler,
                                uint32_t period_us, uint8_t priority);

    // Register a handler firing once, delay_us of sim time from now. Quantized
    // to the next grid step.
    int32_t (*registerOneShot)(void * context, SIL_irq_handler_F handler,
                               uint32_t delay_us, uint8_t priority);

    // Remove an entry permanently (a periodic timer the firmware stops).
    void (*cancel)(void * context, int32_t handle);

    // Mask/unmask one entry, modelling per-IRQ NVIC enable. A disabled entry
    // keeps its schedule; it simply does not dispatch.
    void (*setEnabled)(void * context, int32_t handle, bool enabled);
} SIL_irq_hooks_S;

/* Public Function Declarations */

// Install (copy) the hook vtable, or clear it with NULL. The struct is copied,
// so the caller's pointer need not outlive the call.
void SIL_irq_setHooks(const SIL_irq_hooks_S * const hooks);

// Register a periodic interrupt. A NULL handler or a zero period is rejected
// locally (never reaches the framework).
int32_t SIL_irq_registerPeriodic(SIL_irq_handler_F handler, uint32_t period_us, uint8_t priority);

// Register a one-shot interrupt delay_us from now. A NULL handler is rejected
// locally; a zero delay means "the next grid step".
int32_t SIL_irq_registerOneShot(SIL_irq_handler_F handler, uint32_t delay_us, uint8_t priority);

void SIL_irq_cancel(int32_t handle);
void SIL_irq_setEnabled(int32_t handle, bool enabled);

#endif // SIL_IRQ_H
