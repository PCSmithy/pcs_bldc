#include "app_server.h"
#include "IO_serial.h"
#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "lib_cobs.h"
#include "lib_crc32.h"
#include "lib_protobuf.h"
#include "lib_build_config.h"
#include "unity.h"
#include <string.h>

/* Test fixtures */

static IO_serial_channelConfig_S serialChannelCfg[IO_SERIAL_CHANNEL_COUNT];
static IO_serial_config_S        serialConfig;
static IO_COBSFrame_channelConfig_S frameChannelCfg[IO_COBSFRAME_CHANNEL_COUNT];
static IO_COBSFrame_config_S     frameConfig;
static app_server_config_S       serverConfig;

/* Stub board hooks: settable telemetry, request recorder. */

static board_Telemetry stubTelemetry;
static bool            stubTelemetryValid;
static board_Request   lastRequest;
static uint32_t        requestCalls;

static bool stubBuildTelemetry(board_Telemetry * const telemetry)
{
    *telemetry = stubTelemetry;
    return stubTelemetryValid;
}

static void stubHandleRequest(const board_Request * const request, shared_Response * const response)
{
    lastRequest = *request;
    requestCalls++;
    response->accepted = true;
}

void setUp(void)
{
    HW_USB_sim_reset();
    (void)HW_USB_init();
    HW_USB_sim_setConnected(true);

    stubTelemetry = (board_Telemetry)board_Telemetry_init_zero;
    stubTelemetryValid = true;
    lastRequest = (board_Request)board_Request_init_zero;
    requestCalls = 0U;

    serialChannelCfg[IO_SERIAL_CHANNEL_CDC] =
        (IO_serial_channelConfig_S){ .transport = IO_SERIAL_TRANSPORT_USB_CDC };
    serialConfig = (IO_serial_config_S){
        .channels = serialChannelCfg, .numChannels = IO_SERIAL_CHANNEL_COUNT };
    TEST_ASSERT_TRUE(IO_serial_init(&serialConfig));

    frameChannelCfg[IO_COBSFRAME_CHANNEL_CDC] = (IO_COBSFrame_channelConfig_S){
        .serialChannel = IO_SERIAL_CHANNEL_CDC,
        .maxFrameLen   = IO_COBSFRAME_MAX_PAYLOAD,
    };
    frameConfig = (IO_COBSFrame_config_S){
        .channels = frameChannelCfg, .numChannels = IO_COBSFRAME_CHANNEL_COUNT };
    TEST_ASSERT_TRUE(IO_COBSFrame_init(&frameConfig));

    serverConfig = (app_server_config_S){
        .frame          = IO_COBSFRAME_CHANNEL_CDC,
        .serial         = IO_SERIAL_CHANNEL_CDC,
        .handleRequest  = stubHandleRequest,
        .buildTelemetry = stubBuildTelemetry,
    };
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
}

void tearDown(void) {}

/* ---- wire helpers ---- */

// Frame an encoded envelope and present it as received bytes.
static void injectEnvelope(const shared_Envelope * const env)
{
    uint8_t plain[LIB_PROTOBUF_ENVELOPE_MAX + 4U];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(shared_Envelope_fields, env, plain, LIB_PROTOBUF_ENVELOPE_MAX, &encodedLen));
    const uint32_t crc = lib_crc32_compute(plain, encodedLen);
    plain[encodedLen]      = (uint8_t)(crc & 0xFFU);
    plain[encodedLen + 1U] = (uint8_t)((crc >> 8U) & 0xFFU);
    plain[encodedLen + 2U] = (uint8_t)((crc >> 16U) & 0xFFU);
    plain[encodedLen + 3U] = (uint8_t)((crc >> 24U) & 0xFFU);

    uint8_t wire[600];
    wire[0] = 0x00U;
    size_t cobsLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_encode(plain, encodedLen + 4U, &wire[1], sizeof(wire) - 2U, &cobsLen));
    wire[cobsLen + 1U] = 0x00U;
    HW_USB_sim_injectRx(wire, (uint32_t)(cobsLen + 2U));
}

// Decode every framed envelope the board transmitted, then clear TX (and
// restore the connection the sim reset drops).
static uint32_t collectReplies(shared_Envelope * const out, uint32_t maxOut)
{
    uint8_t wire[512];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    HW_USB_sim_reset();
    HW_USB_sim_setConnected(true);

    uint32_t count = 0U;
    uint32_t segStart = 0U;
    for (uint32_t i = 0U; i <= wireLen; i++)
    {
        if ((i == wireLen) || (wire[i] == 0x00U))
        {
            const uint32_t segLen = i - segStart;
            if ((segLen > 0U) && (count < maxOut))
            {
                uint8_t plain[LIB_PROTOBUF_ENVELOPE_MAX + 4U];
                size_t plainLen = 0U;
                TEST_ASSERT_TRUE(lib_cobs_decode(&wire[segStart], segLen, plain, sizeof(plain), &plainLen));
                TEST_ASSERT_TRUE(plainLen >= 4U);
                out[count] = (shared_Envelope)shared_Envelope_init_zero;
                TEST_ASSERT_TRUE(lib_protobuf_decode(shared_Envelope_fields, plain, plainLen - 4U, &out[count]));
                count++;
            }
            segStart = i + 1U;
        }
    }
    return count;
}

/* ---- init validation ---- */

static void test_init_rejects_bad_config(void)
{
    TEST_ASSERT_FALSE(app_server_init(NULL));

    app_server_config_S bad = serverConfig;
    bad.frame = IO_COBSFRAME_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.serial = IO_SERIAL_CHANNEL_COUNT;
    TEST_ASSERT_FALSE(app_server_init(&bad));
}

/* ---- fw~conn_server_001: request acknowledgement ---- */

// [test->fw~conn_server_001~1]
static void test_ping_accepted_with_request_id(void)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 9U;
    req.which_payload = shared_Envelope_ping_tag;
    injectEnvelope(&req);

    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(9U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);
}

// [test->fw~conn_server_001~1]
// [test->sys~conn_003~1]
static void test_unrecognized_payload_rejected_with_cause(void)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 5U;
    req.which_payload = shared_Envelope_telemetry_tag;   // not a request payload
    injectEnvelope(&req);

    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(5U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_TRUE(strlen(replies[0].payload.response.cause) > 0U);
}

// [test->fw~conn_server_001~1]
static void test_board_request_forwarded_to_hook(void)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 11U;
    req.which_payload = shared_Envelope_board_request_tag;
    req.payload.board_request.which_command = board_Request_set_velocity_tag;
    req.payload.board_request.command.set_velocity.velocity_radps = 5.5f;
    injectEnvelope(&req);

    app_server_run1ms();

    TEST_ASSERT_EQUAL_UINT32(1U, requestCalls);
    TEST_ASSERT_EQUAL(board_Request_set_velocity_tag, lastRequest.which_command);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.5f, lastRequest.command.set_velocity.velocity_radps);

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(11U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);   // the stub accepts
}

// [test->fw~conn_server_001~1]
static void test_board_request_without_hook_rejected(void)
{
    serverConfig.handleRequest = NULL;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 12U;
    req.which_payload = shared_Envelope_board_request_tag;
    req.payload.board_request.which_command = board_Request_clear_fault_tag;
    injectEnvelope(&req);

    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_TRUE(strlen(replies[0].payload.response.cause) > 0U);
}

/* ---- fw~obs_identity_002: identity query ---- */

// [test->fw~obs_identity_002~1]
static void test_identity_request_returns_build_identity(void)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 3U;
    req.which_payload = shared_Envelope_identity_request_tag;
    injectEnvelope(&req);

    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(3U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_identity_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_STRING(LIB_BUILD_IDENTITY, replies[0].payload.identity.build_id);
}

/* ---- fw~obs_status_001: telemetry cadence ---- */

// [test->fw~obs_status_001~1]
// [test->sys~obs_001~1]
static void test_telemetry_published_every_100th_tick(void)
{
    stubTelemetry.timestamp_ms = 777U;
    stubTelemetry.velocity_measured_radps = 12.5f;

    for (uint32_t i = 0U; i < 99U; i++)
    {
        app_server_run1ms();
    }
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));

    app_server_run1ms();   // the 100th pass publishes
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_telemetry_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(777U, replies[0].payload.telemetry.timestamp_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, replies[0].payload.telemetry.velocity_measured_radps);
}

// [test->fw~obs_status_001~1]
static void test_telemetry_skipped_when_hook_declines(void)
{
    stubTelemetryValid = false;
    for (uint32_t i = 0U; i < 200U; i++)
    {
        app_server_run1ms();
    }
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));
}

/* ---- fw~obs_log_001/002: log capture + emission ---- */

static void printString(const char * const s)
{
    for (size_t i = 0U; i < strlen(s); i++)
    {
        app_server_logByte((uint8_t)s[i]);
    }
}

// [test->fw~obs_log_002~1]
// [test->sys~obs_007~1]
static void test_log_strings_emitted_in_order(void)
{
    printString("hello ");
    printString("world");

    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_log_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_STRING("hello world", replies[0].payload.log.text);
}

// [test->fw~obs_log_001~1]
static void test_log_overflow_retains_newest(void)
{
    // 600 patterned bytes into a 512-byte buffer: the emitted stream must be
    // exactly the newest 512, in order.
    uint8_t expected[512];
    for (uint32_t i = 0U; i < 600U; i++)
    {
        const uint8_t byte = (uint8_t)('A' + (i % 26U));
        app_server_logByte(byte);
        if (i >= 88U)
        {
            expected[i - 88U] = byte;
        }
    }

    uint8_t emitted[600];
    size_t emittedLen = 0U;
    shared_Envelope replies[2];
    for (uint32_t pass = 0U; pass < 8U; pass++)
    {
        app_server_run1ms();
        const uint32_t n = collectReplies(replies, 2U);
        for (uint32_t r = 0U; r < n; r++)
        {
            TEST_ASSERT_EQUAL(shared_Envelope_log_tag, replies[r].which_payload);
            const size_t chunk = strlen(replies[r].payload.log.text);
            (void)memcpy(&emitted[emittedLen], replies[r].payload.log.text, chunk);
            emittedLen += chunk;
        }
    }

    TEST_ASSERT_EQUAL_size_t(512U, emittedLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, emitted, 512U);
}

// [test->fw~obs_log_002~1]
static void test_log_buffered_while_disconnected_emitted_on_connect(void)
{
    HW_USB_sim_setConnected(false);
    printString("offline text");
    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));

    HW_USB_sim_setConnected(true);
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_log_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_STRING("offline text", replies[0].payload.log.text);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_bad_config);

    RUN_TEST(test_ping_accepted_with_request_id);
    RUN_TEST(test_unrecognized_payload_rejected_with_cause);
    RUN_TEST(test_board_request_forwarded_to_hook);
    RUN_TEST(test_board_request_without_hook_rejected);

    RUN_TEST(test_identity_request_returns_build_identity);

    RUN_TEST(test_telemetry_published_every_100th_tick);
    RUN_TEST(test_telemetry_skipped_when_hook_declines);

    RUN_TEST(test_log_strings_emitted_in_order);
    RUN_TEST(test_log_overflow_retains_newest);
    RUN_TEST(test_log_buffered_while_disconnected_emitted_on_connect);

    return UNITY_END();
}
