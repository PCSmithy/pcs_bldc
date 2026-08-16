/* Includes */
#include "app_server.h"

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

/* Private Data Definitions */

typedef struct
{
    const app_server_config_S * config;
    uint32_t telemetryDivider;
    ringbuf_t logRing;
    uint8_t logStorage[APP_SERVER_LOG_BUF_BYTES];
    uint8_t rxFrame[LIB_PROTOBUF_ENVELOPE_MAX];
    uint8_t txBytes[LIB_PROTOBUF_ENVELOPE_MAX];
} app_server_data_S;

static app_server_data_S app_server_data;
static app_server_data_S * const data = &app_server_data;

/* Private Function Definitions */

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
    shared_Envelope reply = shared_Envelope_init_zero;
    reply.request_id = request->request_id;

    switch (request->which_payload)
    {
        case shared_Envelope_ping_tag:
            reply.which_payload = shared_Envelope_response_tag;
            reply.payload.response.accepted = true;
            break;

        case shared_Envelope_identity_request_tag:
            reply.which_payload = shared_Envelope_identity_tag;
            (void) strcpy(reply.payload.identity.build_id, LIB_BUILD_IDENTITY);
            break;

        case shared_Envelope_board_request_tag:
            // The board hook owns the verdict; without one, every board
            // command is rejected.
            reply.which_payload = shared_Envelope_response_tag;
            reply.payload.response.accepted = false;
            (void) strcpy(reply.payload.response.cause, "unsupported request");
            if (data->config->handleRequest != NULL)
            {
                data->config->handleRequest(&request->payload.board_request, &reply.payload.response);
            }
            break;

        default:
            reply.which_payload = shared_Envelope_response_tag;
            reply.payload.response.accepted = false;
            (void) strcpy(reply.payload.response.cause, "unsupported request");
            break;
    }

    (void) app_server_private_sendEnvelope(&reply);
}

static void app_server_private_pumpRequests(void)
{
    IO_COBSFrame_run();
    size_t frameLen = 0U;
    while (IO_COBSFrame_receive(data->config->frame, data->rxFrame, sizeof(data->rxFrame), &frameLen))
    {
        shared_Envelope request = shared_Envelope_init_zero;
        if (lib_protobuf_decode(shared_Envelope_fields, data->rxFrame, frameLen, &request))
        {
            app_server_private_handleEnvelope(&request);
        }
        IO_COBSFrame_run();
    }
}

// [impl->fw~obs_status_001~1]
static void app_server_private_publishTelemetry(void)
{
    shared_Envelope env = shared_Envelope_init_zero;
    env.which_payload = shared_Envelope_telemetry_tag;
    if ((data->config->buildTelemetry != NULL) &&
        (data->config->buildTelemetry(&env.payload.telemetry)))
    {
        (void) app_server_private_sendEnvelope(&env);
    }
}

// [impl->fw~obs_log_002~1]
static void app_server_private_drainLog(void)
{
    if ((ringbuf_count(&data->logRing) > 0U) &&
        (IO_serial_txFree(data->config->serial) >= APP_SERVER_LOG_WIRE_RESERVE))
    {
        shared_Envelope env = shared_Envelope_init_zero;
        env.which_payload = shared_Envelope_log_tag;
        char * const text = env.payload.log.text;

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

        (void) app_server_private_sendEnvelope(&env);
    }
}

/* Public Function Definitions */

bool app_server_init(const app_server_config_S * const config)
{
    bool success = false;
    if ((config != NULL) &&
        (config->frame < IO_COBSFRAME_CHANNEL_COUNT) &&
        (config->serial < IO_SERIAL_CHANNEL_COUNT))
    {
        data->config = config;
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
        app_server_private_pumpRequests();

        if (IO_serial_connected(data->config->serial))
        {
            data->telemetryDivider++;
            if (data->telemetryDivider >= APP_SERVER_TELEMETRY_PERIOD_TICKS)
            {
                data->telemetryDivider = 0U;
                app_server_private_publishTelemetry();
            }
            app_server_private_drainLog();
        }
    }
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
