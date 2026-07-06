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
            // CYPD3177 USB-PD sink HPI bus (external, 5.1k pull-ups): 400 kHz
            // fast mode at the 144 MHz kernel clock. Unverified until the C28
            // rework frees the bus — drop to 100 kHz if the rise time proves
            // marginal. TIMINGR is opaque, so the SCL rate is carried
            // separately for the transfer-timeout formula.
            .Init =
            {
                // .Timing           = 0x60715075, // 100kHz
                .Timing           = 0x10E32674, // 400kHz
                // .Timing           = 0x10A30E20, // 1MHz
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
        .sclBitRateHz = 400000U,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 400000U,
        .busNameStr   = "I2C1",
#else
# error "ERROR! HW_I2C_config not defined for build target!"
#endif
    },
    [HW_I2C_BUS_2] =
    {
        .enabled = true,
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        .hi2c =
        {
            .Instance = I2C3,
            // STSPIN32G4 gate-driver bus, bonded in-package to PC8/PC9 (short,
            // internal pull-ups): 1 MHz Fast-mode Plus at the 144 MHz PCLK1
            // kernel clock, bench-verified.
            .Init =
            {
                // .Timing           = 0x60715075, // 100kHz
                // .Timing           = 0x10E32674, // 400kHz
                .Timing           = 0x10A30E20, // 1MHz
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
        .sclBitRateHz = 1000000U,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .transferMode = HW_I2C_TRANSFERMODE_INTERRUPT,
        .sclBitRateHz = 1000000U,
        .busNameStr   = "I2C3",
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
