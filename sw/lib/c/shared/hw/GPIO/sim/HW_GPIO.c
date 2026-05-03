/* Includes */
#include "HW_GPIO.h"

/* Defines */

/* Typedefs */
typedef struct
{
    const HW_GPIO_config_S * config;
    bool initialized;
} HW_GPIO_data_S;

/* Private Function Declarations */

/* Private Data Definitions */

static HW_GPIO_data_S HW_GPIO_data;
static HW_GPIO_data_S * const data = &HW_GPIO_data;

/* Private Function Definitions */

/* Public Function Definitions */
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        // Sim has no real pins to configure — accept the config,
        // remember it (future SIL tests may want to query pin states),
        // and report success. Project policy: init returns bool so
        // main.c handles failure uniformly across targets.
        data->config      = config;
        data->initialized = true;
        ret = true;
    }
    return ret;
}
