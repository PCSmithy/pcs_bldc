/* Includes */
#include "IO_AS5048.h"
#include "HW_SPI.h"

#include "IO_AS5048_channels.h"

/* Defines */

// SPI command frame (MSB first): bit15 = even parity over bits[14:0],
// bit14 = R/W (1 = read), bits[13:0] = register address. Reading the ANGLE
// register (0x3FFF) with the read bit set and even parity yields 0xFFFF.
#define AS5048_CMD_READ_ANGLE  0xFFFFU

// Response frame: bit15 = parity, bit14 = error flag, bits[13:0] = angle.
#define AS5048_RESP_ANGLE_MASK 0x3FFFU
#define AS5048_RESP_ERROR_FLAG 0x4000U

#define AS5048_FRAME_BYTES     2U

typedef struct
{
    uint16_t raw;
    float32_t angle_deg;

} IO_AS5048_channelData_S;

typedef struct
{
    const IO_AS5048_config_S * config;

    IO_AS5048_channelData_S channels[IO_AS5048_CHANNEL_COUNT];
} IO_AS5048_data_S;

static IO_AS5048_data_S IO_AS5048_data;
static IO_AS5048_data_S * const data = &IO_AS5048_data;

/* Private Function Declarations */

static bool IO_AS5048_private_transfer(IO_AS5048_channel_E channel, uint16_t command, uint16_t * response);

/* Private Function Definitions */

// One 16-bit transaction over BUS_1, sent as a big-endian byte pair. The
// HW_SPI driver frames CS around the transfer (assert -> clock 16 bits ->
// deassert), which also satisfies the AS5048's CSn-high-between-frames reset.
static bool IO_AS5048_private_transfer(IO_AS5048_channel_E channel, uint16_t command, uint16_t * response)
{
    uint8_t txData[AS5048_FRAME_BYTES] = { (uint8_t)(command >> 8U), (uint8_t)(command & 0xFFU) };
    uint8_t rxData[AS5048_FRAME_BYTES] = { 0U, 0U };

    const bool ret = HW_SPI_transmitReceive(data->config->channels[channel].spiChannel, txData, rxData, AS5048_FRAME_BYTES);
    if (ret && (response != NULL))
    {
        *response = (uint16_t)(((uint16_t)rxData[0] << 8U) | (uint16_t)rxData[1]);
    }
    return ret;
}

/* Public Function Definitions */
bool IO_AS5048_init(const IO_AS5048_config_S * const config)
{
    bool success = true;

    if (config != NULL)
    {
        for (IO_AS5048_channel_E channel = (IO_AS5048_channel_E)0U; channel < IO_AS5048_CHANNEL_COUNT; channel++)
        {
            success &= config->channels[channel].spiChannel < HW_SPI_CHANNEL_COUNT;
        }
    }

    if (success)
    {
        data->config = config;
    }

    return success;
}

void IO_AS5048_run1ms(void)
{
    if (data->config)
    {
        for (IO_AS5048_channel_E channel = (IO_AS5048_channel_E)0U; channel < IO_AS5048_CHANNEL_COUNT; channel++)
        {
            IO_AS5048_channelData_S * const channelData = &data->channels[channel];

            // The AS5048 pipelines: a command's response comes back on the NEXT
            // frame. Frame 1 issues the read; frame 2 re-issues it and captures
            // the angle response from frame 1.
            uint16_t response = 0U;
            const bool frame1Ok = IO_AS5048_private_transfer(channel, AS5048_CMD_READ_ANGLE, NULL);
            const bool frame2Ok = IO_AS5048_private_transfer(channel, AS5048_CMD_READ_ANGLE, &response);

            if (frame1Ok && frame2Ok && ((response & AS5048_RESP_ERROR_FLAG) == 0U))
            {
                uint16_t raw = (uint16_t)(response & AS5048_RESP_ANGLE_MASK);
                if (data->config->channels[channel].reverse)
                {
                    // Complement the count (wrapping 0 -> 0) so out = 360 - angle.
                    raw = (uint16_t)((AS5048_COUNTS_PER_REV - raw) % AS5048_COUNTS_PER_REV);
                }
                channelData->raw = raw;
                channelData->angle_deg = ((float32_t)raw * 360.0f) / AS5048_COUNTS_PER_REV;
            }

        }
    }
}

bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg)
{
    bool ret = false;
    if ((data->config) && (channel < IO_AS5048_CHANNEL_COUNT))
    {
        if (angleRaw != NULL) { *angleRaw = data->channels[channel].raw; }
        if (angle_deg != NULL) { *angle_deg = data->channels[channel].angle_deg; }

        ret = true; // TODO - could add some fault detection check also...
    }
    return ret;
}
