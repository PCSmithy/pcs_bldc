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

    // Outcome of the most recent _run1ms read on this channel.
    IO_AS5048_status_E status;
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
static bool IO_AS5048_private_evenParityOk(uint16_t frame);

/* Private Function Definitions */

// The AS5048 sets the response frame's bit15 for EVEN parity over the whole
// 16-bit frame, so a valid frame has an even number of set bits. XOR-fold to
// the LSB: 0 = even (valid), 1 = odd (parity error).
static bool IO_AS5048_private_evenParityOk(uint16_t frame)
{
    uint16_t fold = frame;
    fold ^= (uint16_t)(fold >> 8U);
    fold ^= (uint16_t)(fold >> 4U);
    fold ^= (uint16_t)(fold >> 2U);
    fold ^= (uint16_t)(fold >> 1U);
    return ((fold & 1U) == 0U);
}

// One 16-bit transaction over BUS_1, sent as a big-endian byte pair. The
// HW_SPI driver frames CS around the transfer (assert -> clock 16 bits ->
// deassert), which also satisfies the AS5048's CSn-high-between-frames reset.
// [impl->fw~est_encoder_002~1] Each channel maps to its own HW_SPI channel;
// the transaction is addressed purely by logical channel.
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
// [impl->fw~est_encoder_001~1]
bool IO_AS5048_init(const IO_AS5048_config_S * const config)
{
    bool success = false;

    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= IO_AS5048_CHANNEL_COUNT))
    {
        success = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            if (config->channels[channel].spiChannel >= HW_SPI_CHANNEL_COUNT)
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->config = config;
        }
    }

    return success;
}

// [impl->fw~est_encoder_003~1]
void IO_AS5048_run1ms(void)
{
    if (data->config != NULL)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            IO_AS5048_channelData_S * const channelData = &data->channels[channel];

            // The AS5048 pipelines: a command's response comes back on the NEXT
            // frame. Frame 1 issues the read; frame 2 re-issues it and captures
            // the angle response from frame 1.
            uint16_t response = 0U;
            const bool frame1Ok = IO_AS5048_private_transfer((IO_AS5048_channel_E)channel, AS5048_CMD_READ_ANGLE, NULL);
            const bool frame2Ok = IO_AS5048_private_transfer((IO_AS5048_channel_E)channel, AS5048_CMD_READ_ANGLE, &response);

            // [impl->fw~est_encoder_006~1] Accept the frame only if the transfer
            // succeeded, its even parity holds, and the error flag is clear;
            // otherwise hold the last good angle and fault the channel.
            const bool frameValid = (frame1Ok) && (frame2Ok) &&
                                    (IO_AS5048_private_evenParityOk(response)) &&
                                    ((response & AS5048_RESP_ERROR_FLAG) == 0U);

            if (frameValid)
            {
                uint16_t raw = (uint16_t)(response & AS5048_RESP_ANGLE_MASK);
                // [impl->fw~est_encoder_005~1] Reverse: complement the count
                // (wrapping 0 -> 0) so out = 360 - angle.
                if (data->config->channels[channel].reverse)
                {
                    raw = (uint16_t)((AS5048_COUNTS_PER_REV - raw) % AS5048_COUNTS_PER_REV);
                }
                channelData->raw = raw;
                channelData->angle_deg = ((float32_t)raw * 360.0f) / AS5048_COUNTS_PER_REV;
                channelData->status = IO_AS5048_STATUS_OK;
            }
            else
            {
                channelData->status = IO_AS5048_STATUS_FAULT;
            }
        }
    }
}

// [impl->fw~est_encoder_004~1]
bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg)
{
    bool ret = false;
    if ((data->config) && (channel < IO_AS5048_CHANNEL_COUNT))
    {
        if (angleRaw != NULL) { *angleRaw = data->channels[channel].raw; }
        if (angle_deg != NULL) { *angle_deg = data->channels[channel].angle_deg; }

        ret = true;
    }
    return ret;
}

// [impl->fw~est_encoder_006~1]
bool IO_AS5048_getStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E * const out)
{
    bool ret = false;
    if ((out != NULL) && (data->config != NULL) && (channel < IO_AS5048_CHANNEL_COUNT))
    {
        *out = data->channels[channel].status;
        ret = true;
    }
    return ret;
}
