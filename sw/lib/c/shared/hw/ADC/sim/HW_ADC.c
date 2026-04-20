
/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"

/* Defines */
#define HW_ADC_INPUTS_PER_ADC (16U)

/* Typedefs */
typedef struct
{
    uint32_t counts[HW_ADC_INPUTS_PER_ADC];
} HW_ADC_channelData_S;

typedef struct
{
    const HW_ADC_config_S * config;
    HW_ADC_channelData_S channelData[HW_ADC_CHANNEL_COUNT];
} HW_ADC_data_S;

/* Private Function Declarations */

/* Private Data Definitions */
static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

/* Private Function Definitions */

/* Public Function Definitions */

bool HW_ADC_init(const HW_ADC_config_S * config)
{
    (void)config;
    bool success = true;
    // Sim has no real ADC to configure — always succeeds. Project
    // policy: init functions return success/failure; main.c handles.
    if (success)
    {
        data->config = config;
    }
    return success;
}

void HW_ADC_run1ms(void)
{
    if (data->config != NULL)
    {
        // TODO - sample all configured ADC channels.

    }
}
