/* Includes */
#include "lib_cobs.h"

/* Public Function Definitions */

// [impl->fw~conn_proto_002~1]
// The encoder's position between segments: the open block's code slot, the
// write index, and the block's running code.
typedef struct
{
    size_t codeIndex;
    size_t out;
    uint8_t code;
} lib_cobs_private_encoder_S;

static void lib_cobs_private_encodeSegment(lib_cobs_private_encoder_S * const enc,
                                           const uint8_t * const input, size_t len,
                                           uint8_t * const output)
{
    for (size_t i = 0U; i < len; i++)
    {
        if (input[i] == 0U)
        {
            output[enc->codeIndex] = enc->code;
            enc->codeIndex = enc->out;
            enc->out++;
            enc->code = 1U;
        }
        else
        {
            output[enc->out] = input[i];
            enc->out++;
            enc->code++;
            // A full 254-byte block: close it and start the next.
            if (enc->code == 0xFFU)
            {
                output[enc->codeIndex] = enc->code;
                enc->codeIndex = enc->out;
                enc->out++;
                enc->code = 1U;
            }
        }
    }
}

bool lib_cobs_encode2(const uint8_t * const input1, size_t len1,
                      const uint8_t * const input2, size_t len2,
                      uint8_t * const output, size_t outputMax,
                      size_t * const encodedLen)
{
    bool success = false;
    if (((input1 != NULL) || (len1 == 0U)) &&
        ((input2 != NULL) || (len2 == 0U)) &&
        (output != NULL) &&
        (encodedLen != NULL) &&
        (outputMax >= LIB_COBS_ENCODED_MAX(len1 + len2)))
    {
        lib_cobs_private_encoder_S enc = { .codeIndex = 0U, .out = 1U, .code = 1U };
        lib_cobs_private_encodeSegment(&enc, input1, len1, output);
        lib_cobs_private_encodeSegment(&enc, input2, len2, output);
        output[enc.codeIndex] = enc.code;
        *encodedLen = enc.out;
        success = true;
    }
    return success;
}

bool lib_cobs_encode(const uint8_t * const input, size_t len,
                     uint8_t * const output, size_t outputMax,
                     size_t * const encodedLen)
{
    return (input != NULL) && lib_cobs_encode2(input, len, NULL, 0U, output, outputMax, encodedLen);
}

// [impl->fw~conn_proto_002~1]
bool lib_cobs_decode(const uint8_t * const input, size_t len,
                     uint8_t * const output, size_t outputMax,
                     size_t * const decodedLen)
{
    bool success = false;
    if ((input != NULL) && (output != NULL) && (decodedLen != NULL) && (len > 0U))
    {
        bool valid = true;
        size_t in = 0U;
        size_t out = 0U;
        while ((valid) && (in < len))
        {
            const uint8_t code = input[in];
            in++;
            if ((code == 0U) || ((in + ((size_t) code - 1U)) > len))
            {
                valid = false;
            }
            else
            {
                for (uint8_t i = 1U; (valid) && (i < code); i++)
                {
                    if ((input[in] == 0U) || (out >= outputMax))
                    {
                        valid = false;
                    }
                    else
                    {
                        output[out] = input[in];
                        out++;
                        in++;
                    }
                }
                // A block shorter than 254 bytes implies an encoded zero —
                // unless it was the final block.
                if ((valid) && (code != 0xFFU) && (in < len))
                {
                    if (out >= outputMax)
                    {
                        valid = false;
                    }
                    else
                    {
                        output[out] = 0U;
                        out++;
                    }
                }
            }
        }
        if (valid)
        {
            *decodedLen = out;
            success = true;
        }
    }
    return success;
}
