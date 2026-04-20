

/* Includes */
#include "HW_systemClock.h"

/* Defines */

/* Typedefs */

/* Private Function Declarations */

/* Private Data Definitions */

/* Private Function Definitions */

/* Public Function Definitions */
bool HW_systemClock_init(const HW_systemClock_config_S * const config)
{
    (void)config;
    // Sim has no real clock to configure — always succeeds. Project
    // policy: init functions report success/failure as bool; main.c is
    // the single place that decides what to do on failure (calls
    // Error_Handler).
    return true;
}
