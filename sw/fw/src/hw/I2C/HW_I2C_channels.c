/* Includes */

#include "lib_types.h"
#include "lib_utils.h"
#include "lib_build.h"

#include "HW_I2C.h"

/* Defines */

/* Typedefs */

/* Private Data Definitions */

static const HW_I2C_busConfig_S HW_I2C_busConfig[] =
{
    [HW_I2C_BUS_1] =
    {
        .enabled = true,
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        .hi2c =
        {
            .Instance = I2C1,
            // CYPD3177 USB-PD sink HPI bus. TIMINGR from the .ioc (MX_I2C1_Init):
            // 0x60715075 at the 144 MHz I2C1 kernel clock -> ~100 kHz standard
            // mode. TIMINGR is opaque, so the SCL rate is carried separately for
            // the transfer-timeout formula.
            .Init =
            {
                .Timing           = 0x60715075,
                .OwnAddress1      = 0,
                .AddressingMode   = I2C_ADDRESSINGMODE_7BIT,
                .DualAddressMode  = I2C_DUALADDRESS_DISABLE,
                .OwnAddress2      = 0,
                .OwnAddress2Masks = I2C_OA2_NOMASK,
                .GeneralCallMode  = I2C_GENERALCALL_DISABLE,
                .NoStretchMode    = I2C_NOSTRETCH_DISABLE,
            },
        },
        .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 100000U,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 100000U,
        .busNameStr   = "I2C1",
#else
# error "ERROR! HW_I2C_config not defined for build target!"
#endif
    },
};

const HW_I2C_config_S HW_I2C_config =
{
    .buses = HW_I2C_busConfig,
    .numBuses = COUNTOF(HW_I2C_busConfig),
};

/* Private Function Definitions */

/* Public Function Definitions */
