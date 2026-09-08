#pragma once

// Test double for the registration subset of SIL_irq_hooks_S, shared by the sim
// driver suites: the pended path ADC/DMA/SPI use for completions, and the
// periodic path USB uses for its waker. Install it before the driver's init so
// the registration is captured; a recorded handler is an interrupt a test can
// invoke by hand.

/* Includes */
#include "SIL_irq.h"

/* Typedefs */

typedef struct
{
    SIL_irq_handler_F pendedHandler;        // handler the driver registered
    uint32_t          pendedRegisterCalls;
    int32_t           pendedRegisterReturn; // handle registration hands back

    SIL_irq_handler_F periodicHandler;
    uint32_t          periodicRegisterCalls;
    int32_t           periodicRegisterReturn;
    uint32_t          lastPeriodUs;

    int32_t           lastPendHandle;
    uint32_t          pendCalls;
    int32_t           lastCancelHandle;
    uint32_t          cancelCalls;
} SIL_irq_double_S;

/* Public Data Declarations */

extern SIL_irq_double_S SIL_irq_double;

/* Public Function Declarations */

// Install the fake vtable and clear its log; either registration path hands back
// registerReturn.
void SIL_irq_double_install(int32_t registerReturn);
