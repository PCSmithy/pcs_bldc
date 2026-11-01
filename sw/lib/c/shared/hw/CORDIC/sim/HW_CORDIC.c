/* Includes */
#include "HW_CORDIC.h"
#include <math.h>

/* Typedefs */

typedef struct
{
    bool initialized;
} HW_CORDIC_data_S;

/* Private Data Definitions */

static HW_CORDIC_data_S HW_CORDIC_data;
static HW_CORDIC_data_S * const data = &HW_CORDIC_data;

/* Public Function Definitions */

// [impl->fw~hal_cordic_001~1]
bool HW_CORDIC_init(const HW_CORDIC_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        data->initialized = true;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_cordic_002~1]
bool HW_CORDIC_sinCos(float32_t theta_rad, theta_S * const out)
{
    bool ret = false;
    if ((data->initialized) && (out != NULL))
    {
        out->sin_rad = sinf(theta_rad);
        out->cos_rad = cosf(theta_rad);
        ret = true;
    }
    return ret;
}
