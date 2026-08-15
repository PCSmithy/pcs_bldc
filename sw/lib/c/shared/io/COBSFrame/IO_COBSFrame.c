/* Includes */
#include "IO_COBSFrame.h"
#include "lib_crc32.h"

/* Private Data Definitions */

typedef struct
{
    size_t  accumLen;
    bool    overflowed;
    bool    pendingValid;
    size_t  pendingLen;
    uint8_t accum[LIB_COBS_ENCODED_MAX(IO_COBSFRAME_MAX_PAYLOAD + IO_COBSFRAME_CRC_LEN)];
    uint8_t pending[IO_COBSFRAME_MAX_PAYLOAD + IO_COBSFRAME_CRC_LEN];
} IO_COBSFrame_channelData_S;

typedef struct
{
    const IO_COBSFrame_config_S * config;
    IO_COBSFrame_channelData_S channelData[IO_COBSFRAME_CHANNEL_COUNT];
    uint8_t txPlain[IO_COBSFRAME_MAX_PAYLOAD + IO_COBSFRAME_CRC_LEN];
    uint8_t txWire[IO_COBSFRAME_WIRE_MAX(IO_COBSFRAME_MAX_PAYLOAD)];
} IO_COBSFrame_data_S;

static IO_COBSFrame_data_S IO_COBSFrame_data;
static IO_COBSFrame_data_S * const data = &IO_COBSFrame_data;

/* Private Function Definitions */

static void IO_COBSFrame_private_completeSegment(IO_COBSFrame_channel_E channel)
{
    const IO_COBSFrame_channelConfig_S * const cfg = &data->config->channels[channel];
    IO_COBSFrame_channelData_S * const chData = &data->channelData[channel];

    size_t decodedLen = 0U;
    if ((lib_cobs_decode(chData->accum, chData->accumLen,
                         chData->pending, cfg->maxFrameLen + IO_COBSFRAME_CRC_LEN,
                         &decodedLen)) &&
        (decodedLen >= IO_COBSFRAME_CRC_LEN))
    {
        const size_t payloadLen = decodedLen - IO_COBSFRAME_CRC_LEN;
        const uint8_t * const trailer = &chData->pending[payloadLen];
        const uint32_t received = ((uint32_t) trailer[0]) |
                                  (((uint32_t) trailer[1]) << 8U) |
                                  (((uint32_t) trailer[2]) << 16U) |
                                  (((uint32_t) trailer[3]) << 24U);
        if (lib_crc32_compute(chData->pending, payloadLen) == received)
        {
            chData->pendingValid = true;
            chData->pendingLen = payloadLen;
        }
    }
}

// [impl->fw~conn_proto_005~1]
static void IO_COBSFrame_private_pumpChannel(IO_COBSFrame_channel_E channel)
{
    const IO_COBSFrame_channelConfig_S * const cfg = &data->config->channels[channel];
    IO_COBSFrame_channelData_S * const chData = &data->channelData[channel];
    const size_t accumMax = LIB_COBS_ENCODED_MAX(cfg->maxFrameLen + IO_COBSFRAME_CRC_LEN);

    // Byte-at-a-time so a completed frame halts consumption exactly at its
    // delimiter — bytes behind it stay queued in the serial layer.
    bool more = true;
    while ((more) && (!chData->pendingValid))
    {
        uint8_t byte = 0U;
        if (IO_serial_read(cfg->serialChannel, &byte, 1U) == 1U)
        {
            if (byte == 0x00U)
            {
                if ((chData->accumLen > 0U) && (!chData->overflowed))
                {
                    IO_COBSFrame_private_completeSegment(channel);
                }
                chData->accumLen = 0U;
                chData->overflowed = false;
            }
            else
            {
                if (chData->accumLen < accumMax)
                {
                    chData->accum[chData->accumLen] = byte;
                    chData->accumLen++;
                }
                else
                {
                    chData->overflowed = true;
                }
            }
        }
        else
        {
            more = false;
        }
    }
}

/* Public Function Definitions */

// [impl->fw~conn_proto_003~1]
bool IO_COBSFrame_init(const IO_COBSFrame_config_S * const config)
{
    bool success = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= IO_COBSFRAME_CHANNEL_COUNT))
    {
        success = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            const IO_COBSFrame_channelConfig_S * const cfg = &config->channels[channel];
            if ((cfg->serialChannel >= IO_SERIAL_CHANNEL_COUNT) ||
                (cfg->maxFrameLen == 0U) ||
                (cfg->maxFrameLen > IO_COBSFRAME_MAX_PAYLOAD))
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->config = config;
            for (size_t channel = 0U; channel < IO_COBSFRAME_CHANNEL_COUNT; channel++)
            {
                IO_COBSFrame_channelData_S * const chData = &data->channelData[channel];
                chData->accumLen = 0U;
                chData->overflowed = false;
                chData->pendingValid = false;
                chData->pendingLen = 0U;
            }
        }
    }
    return success;
}

void IO_COBSFrame_run(void)
{
    if (data->config != NULL)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            IO_COBSFrame_private_pumpChannel((IO_COBSFrame_channel_E) channel);
        }
    }
}

// [impl->fw~conn_proto_005~1]
bool IO_COBSFrame_receive(IO_COBSFrame_channel_E channel, uint8_t * const buffer,
                      size_t bufferLen, size_t * const frameLen)
{
    bool got = false;
    if ((data->config != NULL) &&
        (channel < IO_COBSFRAME_CHANNEL_COUNT) &&
        (buffer != NULL) &&
        (frameLen != NULL))
    {
        IO_COBSFrame_channelData_S * const chData = &data->channelData[channel];
        if (chData->pendingValid)
        {
            if (bufferLen >= chData->pendingLen)
            {
                for (size_t i = 0U; i < chData->pendingLen; i++)
                {
                    buffer[i] = chData->pending[i];
                }
                *frameLen = chData->pendingLen;
                got = true;
            }
            chData->pendingValid = false;
        }
    }
    return got;
}

// [impl->fw~conn_proto_002~1]
// [impl->fw~conn_proto_004~1]
bool IO_COBSFrame_send(IO_COBSFrame_channel_E channel, const uint8_t * const payload, size_t len)
{
    bool sent = false;
    if ((data->config != NULL) &&
        (channel < IO_COBSFRAME_CHANNEL_COUNT) &&
        ((payload != NULL) || (len == 0U)))
    {
        const IO_COBSFrame_channelConfig_S * const cfg = &data->config->channels[channel];
        if (len <= cfg->maxFrameLen)
        {
            for (size_t i = 0U; i < len; i++)
            {
                data->txPlain[i] = payload[i];
            }
            const uint32_t crc = lib_crc32_compute(data->txPlain, len);
            data->txPlain[len]      = (uint8_t) (crc & 0xFFU);
            data->txPlain[len + 1U] = (uint8_t) ((crc >> 8U) & 0xFFU);
            data->txPlain[len + 2U] = (uint8_t) ((crc >> 16U) & 0xFFU);
            data->txPlain[len + 3U] = (uint8_t) ((crc >> 24U) & 0xFFU);

            size_t encodedLen = 0U;
            if (lib_cobs_encode(data->txPlain, len + IO_COBSFRAME_CRC_LEN,
                                &data->txWire[1], sizeof(data->txWire) - 2U,
                                &encodedLen))
            {
                data->txWire[0] = 0x00U;
                data->txWire[encodedLen + 1U] = 0x00U;
                const size_t wireLen = encodedLen + 2U;
                if (IO_serial_txFree(cfg->serialChannel) >= wireLen)
                {
                    IO_serial_write(cfg->serialChannel, data->txWire, (uint32_t) wireLen);
                    sent = true;
                }
            }
        }
    }
    return sent;
}
