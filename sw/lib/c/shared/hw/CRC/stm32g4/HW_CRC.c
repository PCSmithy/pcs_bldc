/* Includes */
#include "HW_CRC.h"
#include "stm32g4xx_hal.h"

/* Defines */

// The reflected IEEE form on the unit: the standard polynomial, all-ones
// init, input bits reversed per byte for byte-wide feeding, output bits
// reversed, and the result inverted on the way out.
#define HW_CRC_POLY_IEEE   (0x04C11DB7U)
#define HW_CRC_INIT_IEEE   (0xFFFFFFFFU)
#define HW_CRC_XOR_OUT     (0xFFFFFFFFU)

// Check value of the IEEE CRC-32 over "123456789".
#define HW_CRC_CHECK_VALUE (0xCBF43926U)

/* Typedefs */

typedef struct
{
    bool initialized;
} HW_CRC_data_S;

/* Private Data Definitions */

static HW_CRC_data_S HW_CRC_data;
static HW_CRC_data_S * const data = &HW_CRC_data;

/* Private Function Definitions */

static uint32_t HW_CRC_private_compute(const uint8_t * const bytes, size_t len)
{
    CRC->CR |= CRC_CR_RESET;
    for (size_t i = 0U; i < len; i++)
    {
        *(volatile uint8_t *) &CRC->DR = bytes[i];
    }
    return CRC->DR ^ HW_CRC_XOR_OUT;
}

/* Public Function Definitions */

// [impl->fw~hal_crc_001~1]
bool HW_CRC_init(const HW_CRC_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        __HAL_RCC_CRC_CLK_ENABLE();
        CRC->INIT = HW_CRC_INIT_IEEE;
        CRC->POL  = HW_CRC_POLY_IEEE;
        CRC->CR   = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT;
        // The unit's configuration is proven on the check value before any
        // frame trusts it.
        static const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        data->initialized = (HW_CRC_private_compute(check, sizeof(check)) == HW_CRC_CHECK_VALUE);
        ret = data->initialized;
    }
    return ret;
}

// [impl->fw~hal_crc_002~1]
uint32_t HW_CRC_compute32(const uint8_t * const bytes, size_t len)
{
    uint32_t crc = 0U;
    if ((data->initialized) && ((bytes != NULL) || (len == 0U)))
    {
        crc = HW_CRC_private_compute(bytes, len);
    }
    return crc;
}
