/* Includes */
#include "SIL_irq_double.h"

/* Public Data Definitions */

SIL_irq_double_S SIL_irq_double;

/* Private Data Definitions */

static SIL_irq_double_S * const dbl = &SIL_irq_double;

/* Private Function Definitions */

static int32_t SIL_irq_double_private_registerPended(void * context, SIL_irq_handler_F handler, uint8_t priority)
{
    (void)context; (void)priority;
    dbl->pendedHandler = handler;
    dbl->pendedRegisterCalls++;
    return dbl->pendedRegisterReturn;
}

static int32_t SIL_irq_double_private_registerPeriodic(void * context, SIL_irq_handler_F handler,
                                                      uint32_t period_us, uint8_t priority)
{
    (void)context; (void)priority;
    dbl->periodicHandler = handler;
    dbl->lastPeriodUs    = period_us;
    dbl->periodicRegisterCalls++;
    return dbl->periodicRegisterReturn;
}

static void SIL_irq_double_private_pend(void * context, int32_t handle)
{
    (void)context;
    dbl->lastPendHandle = handle;
    dbl->pendCalls++;
}

static void SIL_irq_double_private_cancel(void * context, int32_t handle)
{
    (void)context;
    dbl->lastCancelHandle = handle;
    dbl->cancelCalls++;
}

/* Public Function Definitions */

void SIL_irq_double_install(int32_t registerReturn)
{
    *dbl = (SIL_irq_double_S){
        .pendedRegisterReturn   = registerReturn,
        .periodicRegisterReturn = registerReturn,
        .lastPendHandle         = SIL_IRQ_HANDLE_INVALID,
        .lastCancelHandle       = SIL_IRQ_HANDLE_INVALID,
    };

    const SIL_irq_hooks_S hooks = {
        .registerPeriodic = SIL_irq_double_private_registerPeriodic,
        .registerPended   = SIL_irq_double_private_registerPended,
        .pend             = SIL_irq_double_private_pend,
        .cancel           = SIL_irq_double_private_cancel,
    };
    SIL_irq_setHooks(&hooks);
}
