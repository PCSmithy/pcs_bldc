/* Includes */
#include "IO_SK6805.h"
#include "HW_SPI.h"

/* Private Data Definitions */

const IO_SK6805_config_S IO_SK6805_config =
{
    .spiChannel = HW_SPI_CHANNEL_SK6805_STRING,
    .invert     = true,  // SK6805 data line is driven via an inverting level shifter
};
