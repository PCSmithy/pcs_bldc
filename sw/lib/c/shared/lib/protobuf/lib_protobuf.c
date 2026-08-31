/* Includes */
#include "lib_protobuf.h"
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
