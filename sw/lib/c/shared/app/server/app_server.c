/* Includes */
#include "app_server.h"
#include "app_server_trace.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ringbuf.h"
#include "lib_protobuf.h"
#include "lib_build_config.h"   // project build-identity seam (LIB_BUILD_IDENTITY)

/* Defines */

// Telemetry cadence: one board.Telemetry per this many 1 ms passes
// (fw~obs_status_001).
#define APP_SERVER_TELEMETRY_PERIOD_TICKS (100U) // TODO - make this a board-specific config parameter

// Log capture bound (fw~obs_log_001).
#define APP_SERVER_LOG_BUF_BYTES (512U)

// Longest text chunk per LogText message: the schema field minus its NUL.
#define APP_SERVER_LOG_CHUNK_CHARS (sizeof(((shared_LogText *) 0)->text) - 1U)

// Transmit headroom a full LogText frame needs; the drain holds off (bytes
// stay ring-buffered) rather than popping text a full transport would drop.
#define APP_SERVER_LOG_WIRE_RESERVE ((uint32_t) IO_COBSFRAME_WIRE_MAX(APP_SERVER_LOG_CHUNK_CHARS + 16U))

// Bound on the disconnect-edge RX flush of a dead session's queued bytes.
#define APP_SERVER_RX_DRAIN_MAX_BYTES (4096U)

/* Private Data Definitions */

typedef struct
{
    const app_server_config_S * config;
    bool wasConnected;
    uint32_t telemetryDivider;
    ringbuf_t logRing;
    uint8_t logStorage[APP_SERVER_LOG_BUF_BYTES];
    // RX sized to the frame cap (callback-decoded watches don't count toward
    // the encode bound); TX to the largest envelope the board encodes.
    uint8_t rxFrame[IO_COBSFRAME_MAX_PAYLOAD];
    uint8_t txBytes[LIB_PROTOBUF_ENVELOPE_MAX];
    // Two envelopes are live at once during dispatch, and each is far too
    // large for the 2 KB server-task stack.
    shared_Envelope rxEnvelope;
    shared_Envelope txEnvelope;
} app_server_data_S;

static app_server_data_S app_server_data;
static app_server_data_S * const data = &app_server_data;

/* Private Function Definitions */

static void app_server_private_zeroEnvelope(shared_Envelope * const env)
{
    (void) memset(env, 0, sizeof(*env));
}

static bool app_server_private_sendEnvelope(const shared_Envelope * const env)
{
    bool sent = false;
    size_t encodedLen = 0U;
    if (lib_protobuf_encode(shared_Envelope_fields, env, data->txBytes, sizeof(data->txBytes), &encodedLen))
    {
        sent = IO_COBSFrame_send(data->config->frame, data->txBytes, encodedLen);
    }
    return sent;
}

// [impl->fw~conn_server_001~1]
// [impl->fw~obs_identity_002~1]
static void app_server_private_handleEnvelope(const shared_Envelope * const request)
{
    shared_Envelope * const reply = &data->txEnvelope;
    app_server_private_zeroEnvelope(reply);
    reply->request_id = request->request_id;

    switch (request->which_payload)
    {
        case shared_Envelope_ping_tag:
            reply->which_payload = shared_Envelope_response_tag;
            reply->payload.response.accepted = true;
            break;

        case shared_Envelope_identity_request_tag:
            reply->which_payload = shared_Envelope_identity_tag;
            // Serve the named identity object, so wire and image report the
            // same bytes (and the anchor is always linked).
            (void) strcpy(reply->payload.identity.build_id, lib_build_identityString);
            break;

        case shared_Envelope_board_request_tag:
            // The board hook owns the verdict; without one, every board
            // command is rejected.
            reply->which_payload = shared_Envelope_response_tag;
            reply->payload.response.accepted = false;
            (void) strcpy(reply->payload.response.cause, "unsupported request");
            if (data->config->handleRequest != NULL)
            {
                data->config->handleRequest(&request->payload.board_request, &reply->payload.response);
            }
            break;

        // [impl->fw~conn_trace_006~1] accepted watch lists answer with the
        // capability report; rejections fall back to the Response verdict.
        case shared_Envelope_watch_request_tag:
        {
            trace_TraceStatus status = trace_TraceStatus_init_zero;
            shared_Response verdict = shared_Response_init_zero;
            if (app_server_trace_admit(&status, &verdict))
            {
                reply->which_payload = shared_Envelope_trace_status_tag;
                reply->payload.trace_status = status;
            }
            else
            {
                reply->which_payload = shared_Envelope_response_tag;
                reply->payload.response = verdict;
            }
            break;
        }

        // [impl->fw~conn_trace_006~1]
        case shared_Envelope_trace_status_request_tag:
            reply->which_payload = shared_Envelope_trace_status_tag;
            app_server_trace_status(&reply->payload.trace_status);
            break;

        case shared_Envelope_read_request_tag:
        {
            trace_ReadReply readReply = trace_ReadReply_init_zero;
            shared_Response verdict = shared_Response_init_zero;
            if (app_server_trace_read(&request->payload.read_request, &readReply, &verdict))
            {
                reply->which_payload = shared_Envelope_read_reply_tag;
                reply->payload.read_reply = readReply;
            }
            else
            {
                reply->which_payload = shared_Envelope_response_tag;
                reply->payload.response = verdict;
            }
            break;
        }

        case shared_Envelope_write_request_tag:
            reply->which_payload = shared_Envelope_response_tag;
            reply->payload.response.accepted =
                app_server_trace_write(&request->payload.write_request, &reply->payload.response);
            break;

        default:
            reply->which_payload = shared_Envelope_response_tag;
            reply->payload.response.accepted = false;
            (void) strcpy(reply->payload.response.cause, "unsupported request");
            break;
    }

    (void) app_server_private_sendEnvelope(reply);
}

static void app_server_private_pumpRequests(void)
{
    IO_COBSFrame_run();
    size_t frameLen = 0U;
    while (IO_COBSFrame_receive(data->config->frame, data->rxFrame, sizeof(data->rxFrame), &frameLen))
    {
        shared_Envelope * const request = &data->rxEnvelope;
        app_server_private_zeroEnvelope(request);
        // The payload oneof is memset by nanopb before the chosen arm
        // decodes, so callback fields inside it are armed through the
        // envelope-level hook, not pre-set.
        request->cb_payload.funcs.decode = app_server_trace_envelopeCallback;
        if (lib_protobuf_decode(shared_Envelope_fields, data->rxFrame, frameLen, request))
        {
            app_server_private_handleEnvelope(request);
        }
        else
        {
            // [impl->fw~conn_server_001~1] a CRC-valid but undecodable
            // envelope is still answered (request_id unknowable: 0).
            shared_Envelope * const reply = &data->txEnvelope;
            app_server_private_zeroEnvelope(reply);
            reply->which_payload = shared_Envelope_response_tag;
            reply->payload.response.accepted = false;
            (void) strcpy(reply->payload.response.cause, "decode error");
            (void) app_server_private_sendEnvelope(reply);
        }
        IO_COBSFrame_run();
    }
}

// [impl->fw~obs_status_001~1]
static void app_server_private_publishTelemetry(void)
{
    shared_Envelope * const env = &data->txEnvelope;
    app_server_private_zeroEnvelope(env);
    env->which_payload = shared_Envelope_telemetry_tag;
    if ((data->config->buildTelemetry != NULL) &&
        (data->config->buildTelemetry(&env->payload.telemetry)))
    {
        (void) app_server_private_sendEnvelope(env);
    }
}

// [impl->fw~conn_trace_004~1] emission: buffered ticks leave in capture order
// [impl->fw~conn_trace_005~1] one Samples payload per tick, tick count + data
static void app_server_private_drainSamples(void)
{
    size_t dataLen = 0U;
    while (app_server_trace_peekLen(&dataLen))
    {
        // Hold off (samples stay ring-buffered) while the transport lacks
        // room for the whole frame; 24 covers the envelope's tags, lengths,
        // and tick varint ahead of the framing overhead.
        const uint32_t reserve = (uint32_t) IO_COBSFRAME_WIRE_MAX(dataLen + 24U);
        if (IO_serial_txFree(data->config->serial) < reserve)
        {
            break;
        }
        shared_Envelope * const env = &data->txEnvelope;
        app_server_private_zeroEnvelope(env);
        env->which_payload = shared_Envelope_samples_tag;
        size_t poppedLen = 0U;
        if (!app_server_trace_pop(&env->payload.samples.tick_ms,
                                  env->payload.samples.data.bytes,
                                  sizeof(env->payload.samples.data.bytes),
                                  &poppedLen))
        {
            break;
        }
        env->payload.samples.data.size = (pb_size_t) poppedLen;
        (void) app_server_private_sendEnvelope(env);
    }
}

// [impl->fw~obs_log_002~1]
static void app_server_private_drainLog(void)
{
    if ((ringbuf_count(&data->logRing) > 0U) &&
        (IO_serial_txFree(data->config->serial) >= APP_SERVER_LOG_WIRE_RESERVE))
    {
        shared_Envelope * const env = &data->txEnvelope;
        app_server_private_zeroEnvelope(env);
        env->which_payload = shared_Envelope_log_tag;
        char * const text = env->payload.log.text;

        size_t chars = 0U;
        uint8_t byte = 0U;
        taskENTER_CRITICAL();
        while ((chars < APP_SERVER_LOG_CHUNK_CHARS) && (ringbuf_pop(&data->logRing, &byte)))
        {
            text[chars] = (char) byte;
            chars++;
        }
        taskEXIT_CRITICAL();
        text[chars] = '\0';

        (void) app_server_private_sendEnvelope(env);
    }
}

/* Public Function Definitions */

bool app_server_init(const app_server_config_S * const config)
{
    bool success = false;
    if ((config != NULL) &&
        (config->frame < IO_COBSFRAME_CHANNEL_COUNT) &&
        (config->serial < IO_SERIAL_CHANNEL_COUNT) &&
        (app_server_trace_init(config)))
    {
        data->config = config;
        data->wasConnected = false;
        data->telemetryDivider = 0U;
        ringbuf_init(&data->logRing, data->logStorage, sizeof(data->logStorage));
        success = true;
    }
    return success;
}

// [impl->fw~obs_status_001~1]
void app_server_run1ms(void)
{
    if (data->config != NULL)
    {
        // Everything gates on an open host connection — including the request
        // pump; the pre-configuration CDC read path is unsafe (see HW_USB.c).
        const bool connected = IO_serial_isConnected(data->config->serial);
        if (connected)
        {
            app_server_private_pumpRequests();

            data->telemetryDivider++;
            if (data->telemetryDivider >= APP_SERVER_TELEMETRY_PERIOD_TICKS)
            {
                data->telemetryDivider = 0U;
                app_server_private_publishTelemetry();
            }
            app_server_private_drainLog();
            app_server_private_drainSamples();
        }
        else if (data->wasConnected)
        {
            // [impl->fw~conn_trace_003~1] the watch list dies with the port —
            // and so does any half-received or held frame from that session.
            app_server_trace_clear();
            IO_COBSFrame_reset(data->config->frame);
            uint8_t discard[16];
            uint32_t drained = 0U;
            while ((IO_serial_read(data->config->serial, discard, sizeof(discard)) > 0U) &&
                   (drained < APP_SERVER_RX_DRAIN_MAX_BYTES))
            {
                drained += (uint32_t) sizeof(discard);
            }
        }
        else
        {
            // Disconnected steady state: nothing to serve.
        }
        data->wasConnected = connected;
    }
}

// [impl->fw~conn_trace_004~1]
void app_server_sample1ms(void)
{
    app_server_trace_sample1ms();
}

// [impl->fw~obs_log_001~1]
void app_server_logByte(uint8_t byte)
{
    if (data->config != NULL)
    {
        taskENTER_CRITICAL();
        if (!ringbuf_push(&data->logRing, byte))
        {
            uint8_t discarded = 0U;
            (void) ringbuf_pop(&data->logRing, &discarded);
            (void) ringbuf_push(&data->logRing, byte);
        }
        taskEXIT_CRITICAL();
    }
}
