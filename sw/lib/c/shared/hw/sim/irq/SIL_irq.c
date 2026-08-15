/* Includes */

#include "lib_types.h"

#include "SIL_irq.h"

/* Defines */

/* Typedefs */

typedef struct
{
    SIL_irq_hooks_S hooks;
    bool installed;
} SIL_irq_data_S;

/* Private Data Definitions */

static SIL_irq_data_S SIL_irq_data;
static SIL_irq_data_S * const data = &SIL_irq_data;

/* Public Function Definitions */

void SIL_irq_setHooks(const SIL_irq_hooks_S * const hooks)
{
    if (hooks != NULL)
    {
        data->hooks     = *hooks;
        data->installed = true;
    }
    else
    {
        data->hooks     = (SIL_irq_hooks_S){ 0 };
        data->installed = false;
    }
}

int32_t SIL_irq_registerPeriodic(SIL_irq_handler_F handler, uint32_t period_us, uint8_t priority)
{
    int32_t handle = SIL_IRQ_HANDLE_INVALID;
    // A zero period would come due every step forever, so it is refused here
    // rather than handed to the framework.
    if ((data->installed) &&
        (data->hooks.registerPeriodic != NULL) &&
        (handler != NULL) &&
        (period_us > 0U))
    {
        handle = data->hooks.registerPeriodic(data->hooks.context, handler, period_us, priority);
    }
    return handle;
}

int32_t SIL_irq_registerOneShot(SIL_irq_handler_F handler, uint32_t delay_us, uint8_t priority)
{
    int32_t handle = SIL_IRQ_HANDLE_INVALID;
    if ((data->installed) &&
        (data->hooks.registerOneShot != NULL) &&
        (handler != NULL))
    {
        handle = data->hooks.registerOneShot(data->hooks.context, handler, delay_us, priority);
    }
    return handle;
}

void SIL_irq_cancel(int32_t handle)
{
    if ((handle >= 0) &&
        (data->installed) &&
        (data->hooks.cancel != NULL))
    {
        data->hooks.cancel(data->hooks.context, handle);
    }
}

void SIL_irq_setEnabled(int32_t handle, bool enabled)
{
    if ((handle >= 0) &&
        (data->installed) &&
        (data->hooks.setEnabled != NULL))
    {
        data->hooks.setEnabled(data->hooks.context, handle, enabled);
    }
}
