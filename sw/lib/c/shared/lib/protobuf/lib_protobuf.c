/* Includes */
#include "lib_protobuf.h"
#include <string.h>
#include "pb_encode.h"
#include "pb_decode.h"

/* Public Function Definitions */

// [impl->fw~conn_proto_001~1]
bool lib_protobuf_encode(const pb_msgdesc_t * const fields, const void * const message,
                         uint8_t * const buffer, size_t bufferLen,
                         size_t * const encodedLen)
{
    bool success = false;
    if ((fields != NULL) && (message != NULL) && (buffer != NULL) && (encodedLen != NULL))
    {
        pb_ostream_t stream = pb_ostream_from_buffer(buffer, bufferLen);
        if (pb_encode(&stream, fields, message))
        {
            *encodedLen = stream.bytes_written;
            success = true;
        }
    }
    return success;
}

// Varint bytes of value into out (out holds at least 5); returns the count.
static size_t lib_protobuf_private_varint(uint32_t value, uint8_t * const out)
{
    size_t n = 0U;
    uint32_t v = value;
    do
    {
        out[n] = (uint8_t) ((v & 0x7FU) | ((v >= 0x80U) ? 0x80U : 0U));
        n++;
        v >>= 7U;
    } while (v != 0U);
    return n;
}

// [impl->fw~conn_proto_001~1]
bool lib_protobuf_encodeEnvelope(uint32_t requestId, uint32_t payloadTag,
                                 const pb_msgdesc_t * const payloadFields, const void * const payload,
                                 uint8_t * const buffer, size_t bufferLen,
                                 size_t * const encodedLen)
{
    // Header worst case: request_id key+varint (6) and payload key (5) + length (5).
    const size_t headerMax = 16U;
    bool success = false;
    if ((payloadFields != NULL) && (payload != NULL) && (buffer != NULL) && (encodedLen != NULL) &&
        (bufferLen > headerMax))
    {
        // The payload lands past the worst-case header; the real header is
        // then written back-to-back in front of it and the whole moved down.
        pb_ostream_t stream = pb_ostream_from_buffer(&buffer[headerMax], bufferLen - headerMax);
        if (pb_encode(&stream, payloadFields, payload))
        {
            uint8_t header[16];
            size_t n = 0U;
            if (requestId != 0U)
            {
                header[n] = 0x08U;   // field 1, varint
                n++;
                n += lib_protobuf_private_varint(requestId, &header[n]);
            }
            n += lib_protobuf_private_varint((payloadTag << 3U) | 2U, &header[n]);
            n += lib_protobuf_private_varint((uint32_t) stream.bytes_written, &header[n]);
            (void) memmove(&buffer[n], &buffer[headerMax], stream.bytes_written);
            (void) memcpy(buffer, header, n);
            *encodedLen = n + stream.bytes_written;
            success = true;
        }
    }
    return success;
}

// [impl->fw~conn_proto_001~1]
bool lib_protobuf_decode(const pb_msgdesc_t * const fields,
                         const uint8_t * const bytes, size_t len,
                         void * const message)
{
    bool success = false;
    if ((fields != NULL) && (bytes != NULL) && (message != NULL))
    {
        pb_istream_t stream = pb_istream_from_buffer(bytes, len);
        success = pb_decode(&stream, fields, message);
    }
    return success;
}
