/* Includes */
#include "IO_i2c.h"
#include "HW_I2C.h"
#include "lib_utils.h"

/* Defines */


/* Private Function Declarations */

/* Private Data Declarations */
static const IO_i2c_deviceConfig_S IO_i2c_deviceConfig[] =
{
    [IO_I2C_DEVICE_CYPD3177] =
    {
        // CYPD3177 USB-PD sink HPI: 7-bit address 0x08, 16-bit register
        // offsets sent low byte first (HPI convention).
        .bus = HW_I2C_BUS_1,
        .devAddr7 = 0x08U,
        .memAddrSize = HW_I2C_MEMADDR_SIZE_16BIT_LSBFIRST,
    },
};

const IO_i2c_config_S IO_i2c_config =
{
    .devices = IO_i2c_deviceConfig,
    .numDevices = COUNTOF(IO_i2c_deviceConfig),
};

/* Private Function Definitions */


/* Public Function Definitions */
