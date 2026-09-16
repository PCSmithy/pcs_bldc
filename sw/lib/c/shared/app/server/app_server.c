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

// Envelope bytes a Samples message costs beyond its data: the envelope tag and
// length plus the period, first-cycle, count, and data-field varints. The
// framing rest of fw~conn_trace_005's W is IO_COBSFRAME_WIRE_MAX's own.
#define APP_SERVER_SAMPLES_ENVELOPE_OVERHEAD (19U)

// Link-throughput-test bounds (fw~conn_server_005). The payload cap is the
// schema's; the frame cap keeps a runaway request finite.
#define APP_SERVER_LINK_TEST_MAX_PAYLOAD (sizeof(((shared_LinkTestFrame *) 0)->payload.bytes))
#define APP_SERVER_LINK_TEST_MAX_FRAMES  (1000000U)

/* Private Data Definitions */

typedef struct
{
    const app_server_config_S * config;
    bool wasConnected;
    uint32_t telemetryDivider;
    ringbuf_t logRing;
    uint8_t logStorage[APP_SERVER_LOG_BUF_BYTES];
    // Link throughput test; remaining == 0 means no test is running, so the
    // count reaching zero is what frees the service for the next request.
    uint32_t linkTestRemaining;
    uint32_t linkTestSeq;
    uint32_t linkTestPayloadBytes;
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

// [impl->fw~conn_server_005~1] admission: bounds, then exclusivity.
static bool app_server_private_admitLinkTest(const shared_LinkTestRequest * const request,
                                             shared_Response * const response)
{
    bool accepted = false;
    if ((request->payload_bytes < 1U) || (request->payload_bytes > APP_SERVER_LINK_TEST_MAX_PAYLOAD))
    {
        (void) strcpy(response->cause, "payload_bytes out of range");
    }
    else if ((request->frame_count < 1U) || (request->frame_count > APP_SERVER_LINK_TEST_MAX_FRAMES))
    {
        (void) strcpy(response->cause, "frame_count out of range");
    }
    else if (data->linkTestRemaining > 0U)
    {
        (void) strcpy(response->cause, "link test already running");
    }
    else
    {
        data->linkTestSeq = 0U;
        data->linkTestPayloadBytes = request->payload_bytes;
        data->linkTestRemaining = request->frame_count;
        accepted = true;
    }
    response->accepted = accepted;
    return accepted;
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

        // [impl->fw~conn_server_005~1]
        case shared_Envelope_link_test_request_tag:
            reply->which_payload = shared_Envelope_response_tag;
            (void) app_server_private_admitLinkTest(&request->payload.link_test_request,
                                                    &reply->payload.response);
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

// [impl->fw~conn_trace_009~1] each group's buffered records leave in capture
// order, consecutive records of one group sharing a message
// [impl->fw~conn_trace_005~1] one Samples per message: the group's period, its
// first record's cycle index, the record count, and the concatenated data
static void app_server_private_drainSamples(void)
{
    bool progressing = true;
    while (progressing)
    {
        uint32_t group = 0U;
        uint32_t cycle = 0U;
        size_t recordLen = 0U;
        progressing = false;
        if (app_server_trace_peek(&group, &cycle, &recordLen))
        {
            const uint32_t periodCycles = app_server_trace_groupPeriodCycles(group);
            shared_Envelope * const env = &data->txEnvelope;
            app_server_private_zeroEnvelope(env);
            env->which_payload = shared_Envelope_samples_tag;
            trace_Samples * const samples = &env->payload.samples;
            samples->period_cycles = periodCycles;
            samples->first_cycle = cycle;

            size_t used = 0U;
            uint32_t nextCycle = cycle;
            bool batching = true;
            while (batching)
            {
                // Hold off (records stay ring-buffered) while the message or
                // the transport lacks room for one more record.
                const uint32_t reserve = (uint32_t) IO_COBSFRAME_WIRE_MAX(
                    used + recordLen + APP_SERVER_SAMPLES_ENVELOPE_OVERHEAD);
                if (((used + recordLen) > sizeof(samples->data.bytes)) ||
                    (IO_serial_txFree(data->config->serial) < reserve) ||
                    (!app_server_trace_pop(&samples->data.bytes[used],
                                           sizeof(samples->data.bytes) - used)))
                {
                    batching = false;
                }
                else
                {
                    used += recordLen;
                    samples->count++;
                    nextCycle += periodCycles;

                    // Only a run of one group's records whose cycle indices
                    // still step by its period can share the message: a gap
                    // left by an overflow starts a new one.
                    uint32_t nextGroup = 0U;
                    uint32_t peekedCycle = 0U;
                    size_t peekedLen = 0U;
                    batching = (app_server_trace_peek(&nextGroup, &peekedCycle, &peekedLen)) &&
                               (nextGroup == group) &&
                               (peekedCycle == nextCycle);
                    recordLen = peekedLen;
                }
            }

            if (samples->count > 0U)
            {
                // The records are already popped: a failed send drops them, and
                // the host sees the loss as a gap in the cycle indices.
                samples->data.size = (pb_size_t) used;
                (void) app_server_private_sendEnvelope(env);
                progressing = true;
            }
        }
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

// [impl->fw~conn_server_005~1] last drain of the pass, measuring only the room
// the real services leave. IO_COBSFrame_send takes whole frames or none, so a
// short pass stops mid-count and resumes next pass.
static void app_server_private_drainLinkTest(void)
{
    bool sent = true;
    while ((data->linkTestRemaining > 0U) && sent)
    {
        shared_Envelope * const env = &data->txEnvelope;
        app_server_private_zeroEnvelope(env);
        env->which_payload = shared_Envelope_link_test_frame_tag;
        env->payload.link_test_frame.seq = data->linkTestSeq;
        env->payload.link_test_frame.payload.size = (pb_size_t) data->linkTestPayloadBytes;
        for (uint32_t i = 0U; i < data->linkTestPayloadBytes; i++)
        {
            env->payload.link_test_frame.payload.bytes[i] = (uint8_t) ((data->linkTestSeq + i) & 0xFFU);
        }

        sent = app_server_private_sendEnvelope(env);
        if (sent)
        {
            data->linkTestSeq++;
            data->linkTestRemaining--;
        }
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
        data->linkTestRemaining = 0U;
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
            app_server_private_drainLinkTest();
        }
        else if (data->wasConnected)
        {
            // [impl->fw~conn_trace_003~1] the watch list dies with the port —
            // and so does any half-received or held frame from that session.
            // [impl->fw~conn_server_005~1] a link test is abandoned with it.
            app_server_trace_clear();
            data->linkTestRemaining = 0U;
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
void app_server_sampleCycle(void)
{
    app_server_trace_sampleCycle();
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
