/* Includes */
#include "lib_crc32.h"

/* Defines */

// Reflected form of the IEEE 802.3 polynomial 0x04C11DB7.
#define LIB_CRC32_POLY_REFLECTED  0xEDB88320U

/* Public Function Definitions */

// [impl->fw~conn_proto_002~1]
uint32_t lib_crc32_compute(const uint8_t * const bytes, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    if (bytes != NULL)
    {
        for (size_t i = 0U; i < len; i++)
        {
            crc ^= bytes[i];
            for (uint8_t bit = 0U; bit < 8U; bit++)
            {
                crc = (((crc & 1U) != 0U)) ? ((crc >> 1U) ^ LIB_CRC32_POLY_REFLECTED) : (crc >> 1U);
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}
