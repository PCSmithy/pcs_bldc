/* Includes */
#include "HW_CRC.h"
#include "lib_crc32.h"

/* Typedefs */

typedef struct
{
    bool initialized;
} HW_CRC_data_S;

/* Private Data Definitions */

static HW_CRC_data_S HW_CRC_data;
static HW_CRC_data_S * const data = &HW_CRC_data;

/* Public Function Definitions */

// [impl->fw~hal_crc_001~1]
bool HW_CRC_init(const HW_CRC_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        data->initialized = true;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_crc_002~1]
uint32_t HW_CRC_compute32(const uint8_t * const bytes, size_t len)
{
    uint32_t crc = 0U;
    if ((data->initialized) && ((bytes != NULL) || (len == 0U)))
    {
        crc = lib_crc32_compute(bytes, len);
    }
    return crc;
}
