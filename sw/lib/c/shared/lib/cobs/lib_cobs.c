/* Includes */
#include "lib_cobs.h"

/* Public Function Definitions */

// [impl->fw~conn_proto_002~1]
bool lib_cobs_encode(const uint8_t * const input, size_t len,
                     uint8_t * const output, size_t outputMax,
                     size_t * const encodedLen)
{
    bool success = false;
    if ((input != NULL) &&
        (output != NULL) &&
        (encodedLen != NULL) &&
        (outputMax >= LIB_COBS_ENCODED_MAX(len)))
    {
        size_t codeIndex = 0U;
        size_t out = 1U;
        uint8_t code = 1U;
        for (size_t i = 0U; i < len; i++)
        {
            if (input[i] == 0U)
            {
                output[codeIndex] = code;
                codeIndex = out;
                out++;
                code = 1U;
            }
            else
            {
                output[out] = input[i];
                out++;
                code++;
                // A full 254-byte block: close it and start the next.
                if (code == 0xFFU)
                {
                    output[codeIndex] = code;
                    codeIndex = out;
                    out++;
                    code = 1U;
                }
            }
        }
        output[codeIndex] = code;
        *encodedLen = out;
        success = true;
    }
    return success;
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
