#include "app_server.h"
#include "IO_serial.h"
#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "lib_cobs.h"
#include "lib_crc32.h"
#include "lib_protobuf.h"
#include "lib_utils.h"
#include "lib_build_config.h"
#include "pb_encode.h"
#include "unity.h"
#include <string.h>

/* Test fixtures */

static IO_serial_channelConfig_S serialChannelCfg[IO_SERIAL_CHANNEL_COUNT];
static IO_serial_config_S        serialConfig;
static IO_COBSFrame_channelConfig_S frameChannelCfg[IO_COBSFRAME_CHANNEL_COUNT];
static IO_COBSFrame_config_S     frameConfig;
static app_server_config_S       serverConfig;

// Trace fixtures: a word-aligned test memory backing the protocol range
// starting at TRACE_TEST_BASE (small on purpose — short address varints keep
// even a capacity-filling watch list inside the frame cap). Writable is the
// front half of readable, so not-writable-but-readable spans exist.
#define TRACE_TEST_BASE       (0x1000U)
#define TRACE_WATCH_CAPACITY  (8U)
#define TRACE_RAM_BUDGET      (256U)
#define TRACE_LINK_BUDGET     (480000U)

static uint32_t traceMemory[64];
static app_server_watch_S traceWatchStorage[2U * TRACE_WATCH_CAPACITY];
static uint8_t traceSampleStorage[APP_SERVER_TRACE_STORAGE_BYTES(TRACE_RAM_BUDGET)];
static const app_server_region_S traceReadableRegions[] = {
    { .start = TRACE_TEST_BASE, .length = sizeof(traceMemory), .base = (uintptr_t) traceMemory },
};
static const app_server_region_S traceWritableRegions[] = {
    { .start = TRACE_TEST_BASE, .length = sizeof(traceMemory) / 2U, .base = (uintptr_t) traceMemory },
};

// Oversized fixtures for the list-level admission bounds (Samples capacity)
// and the max-size Samples frame, unreachable at the default capacity of 8.
#define TRACE_BIG_WATCH_CAPACITY (80U)
#define TRACE_BIG_RAM_BUDGET     (1024U)
static app_server_watch_S traceBigWatchStorage[2U * TRACE_BIG_WATCH_CAPACITY];
static uint8_t traceBigSampleStorage[APP_SERVER_TRACE_STORAGE_BYTES(TRACE_BIG_RAM_BUDGET)];

// A ring deep enough to hold more wire bytes than the 2 KB sim transport takes
// in one pass, so a drain can stop partway and leave records behind it.
#define TRACE_DEEP_RAM_BUDGET (6144U)
static uint8_t traceDeepSampleStorage[APP_SERVER_TRACE_STORAGE_BYTES(TRACE_DEEP_RAM_BUDGET)];

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

// Sampler-mask recorder: the trace-write bracket of fw~conn_trace_008.
static uint32_t samplerMaskCalls;
static bool     samplerMasked;
static uint32_t traceWordWhileMasked;
static uint32_t traceWordWhileUnmasked;

static void stubSetSamplerMasked(bool masked)
{
    samplerMasked = masked;
    samplerMaskCalls++;
    if (masked)
    {
        traceWordWhileMasked = traceMemory[2];
    }
    else
    {
        traceWordWhileUnmasked = traceMemory[2];
    }
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
    samplerMaskCalls = 0U;
    samplerMasked = false;
    traceWordWhileMasked = 0U;
    traceWordWhileUnmasked = 0U;

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

    memset(traceMemory, 0, sizeof(traceMemory));
    serverConfig = (app_server_config_S){
        .frame          = IO_COBSFRAME_CHANNEL_CDC,
        .serial         = IO_SERIAL_CHANNEL_CDC,
        .readableRegions      = traceReadableRegions,
        .readableRegionCount  = 1U,
        .writableRegions      = traceWritableRegions,
        .writableRegionCount  = 1U,
        .watchStorage         = traceWatchStorage,
        .watchCapacity        = TRACE_WATCH_CAPACITY,
        .sampleStorage        = traceSampleStorage,
        .sampleRamBudgetBytes = TRACE_RAM_BUDGET,
        .linkBudgetBytesPerS  = TRACE_LINK_BUDGET,
        .handleRequest  = stubHandleRequest,
        .buildTelemetry = stubBuildTelemetry,
        .setSamplerMasked = stubSetSamplerMasked,
    };
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
}

void tearDown(void) {}

/* ---- wire helpers ---- */

// Frame an encoded envelope and present it as received bytes. Sized to the
// frame cap, not the board's encode bound: a callback-encoded watch list can
// outgrow LIB_PROTOBUF_ENVELOPE_MAX.
static void injectEnvelope(const shared_Envelope * const env)
{
    uint8_t plain[IO_COBSFRAME_MAX_PAYLOAD + 4U];
    size_t encodedLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(shared_Envelope_fields, env, plain, IO_COBSFRAME_MAX_PAYLOAD, &encodedLen));
    const uint32_t crc = lib_crc32_compute(plain, encodedLen);
    plain[encodedLen]      = (uint8_t)(crc & 0xFFU);
    plain[encodedLen + 1U] = (uint8_t)((crc >> 8U) & 0xFFU);
    plain[encodedLen + 2U] = (uint8_t)((crc >> 16U) & 0xFFU);
    plain[encodedLen + 3U] = (uint8_t)((crc >> 24U) & 0xFFU);

    uint8_t wire[IO_COBSFRAME_WIRE_MAX(IO_COBSFRAME_MAX_PAYLOAD + 4U)];
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
    uint8_t wire[2048];
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

// Clear the sim transport (and restore the connection the reset drops), so the
// next pass starts on an empty, freshly counted transmit path.
static void resetTx(void)
{
    HW_USB_sim_reset();
    HW_USB_sim_setConnected(true);
}

// Pack the transport down to `keepFree` bytes of free transmit capacity.
static void packTxTo(uint32_t keepFree)
{
    uint8_t zeros[256] = { 0U };
    while (IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) > keepFree)
    {
        const uint32_t excess = IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) - keepFree;
        IO_serial_write(IO_SERIAL_CHANNEL_CDC, zeros,
                        (excess < sizeof(zeros)) ? excess : (uint32_t) sizeof(zeros));
    }
}

// Room for the reply reserve plus exactly one full 16-record Samples frame.
#define TRACE_ONE_MESSAGE_FREE \
    ((uint32_t) IO_COBSFRAME_WIRE_MAX(LIB_PROTOBUF_ENVELOPE_MAX) + \
     (uint32_t) IO_COBSFRAME_WIRE_MAX(256U + 19U) + 8U)

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

// [test->fw~conn_server_001~1] a CRC-valid but undecodable envelope is still
// answered: rejection with a cause, request_id 0.
static void test_undecodable_frame_rejected_with_cause(void)
{
    uint8_t plain[7];
    plain[0] = 0xFFU;   // field 31, wire type 7: invalid protobuf
    plain[1] = 0xFFU;
    plain[2] = 0xFFU;
    const uint32_t crc = lib_crc32_compute(plain, 3U);
    plain[3] = (uint8_t) (crc & 0xFFU);
    plain[4] = (uint8_t) ((crc >> 8U) & 0xFFU);
    plain[5] = (uint8_t) ((crc >> 16U) & 0xFFU);
    plain[6] = (uint8_t) ((crc >> 24U) & 0xFFU);

    uint8_t wire[16];
    wire[0] = 0x00U;
    size_t cobsLen = 0U;
    TEST_ASSERT_TRUE(lib_cobs_encode(plain, sizeof(plain), &wire[1], sizeof(wire) - 2U, &cobsLen));
    wire[cobsLen + 1U] = 0x00U;
    HW_USB_sim_injectRx(wire, (uint32_t) (cobsLen + 2U));

    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].request_id);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, "decode"));
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

/* ---- trace helpers ---- */

typedef struct
{
    const trace_Watch * items;
    uint32_t count;
} watchList_S;

static watchList_S injectedWatches;

static bool encodeWatchList(pb_ostream_t * stream, const pb_field_t * field, void * const * arg)
{
    const watchList_S * const list = (const watchList_S *) *arg;
    bool ok = true;
    for (uint32_t i = 0U; (ok) && (i < list->count); i++)
    {
        ok = (pb_encode_tag_for_field(stream, field)) &&
             (pb_encode_submessage(stream, trace_Watch_fields, &list->items[i]));
    }
    return ok;
}

static void injectWatchRequest(uint32_t requestId, const trace_Watch * const watches, uint32_t count)
{
    injectedWatches.items = watches;
    injectedWatches.count = count;
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = requestId;
    req.which_payload = shared_Envelope_watch_request_tag;
    req.payload.watch_request.watches.funcs.encode = encodeWatchList;
    req.payload.watch_request.watches.arg = &injectedWatches;
    injectEnvelope(&req);
}

// Install a list, asserting acceptance, and return the TraceStatus reply.
static trace_TraceStatus installWatches(const trace_Watch * const watches, uint32_t count)
{
    injectWatchRequest(77U, watches, count);
    app_server_run1ms();
    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_trace_status_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(77U, replies[0].request_id);
    return replies[0].payload.trace_status;
}

static void expectWatchRejection(const trace_Watch * const watches, uint32_t count, const char * const causeSubstring)
{
    injectWatchRequest(78U, watches, count);
    app_server_run1ms();
    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, causeSubstring));
}

static void useBigTraceConfig(void)
{
    serverConfig.watchStorage         = traceBigWatchStorage;
    serverConfig.watchCapacity        = TRACE_BIG_WATCH_CAPACITY;
    serverConfig.sampleStorage        = traceBigSampleStorage;
    serverConfig.sampleRamBudgetBytes = TRACE_BIG_RAM_BUDGET;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
}

static void useDeepTraceConfig(void)
{
    serverConfig.sampleStorage        = traceDeepSampleStorage;
    serverConfig.sampleRamBudgetBytes = TRACE_DEEP_RAM_BUDGET;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
}

// Ask for the capability report out of band and return it.
static trace_TraceStatus requestTraceStatus(uint32_t requestId)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = requestId;
    req.which_payload = shared_Envelope_trace_status_request_tag;
    injectEnvelope(&req);
    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_trace_status_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(requestId, replies[0].request_id);
    return replies[0].payload.trace_status;
}

// The four one-cycle 4-byte spans of a 16-byte record — the batching cases of
// fw~conn_trace_009 are all written against them.
static void installFourFastWatches(void)
{
    trace_Watch watches[4];
    for (uint32_t i = 0U; i < 4U; i++)
    {
        watches[i] = (trace_Watch){
            .address = TRACE_TEST_BASE + (i * 4U), .size = 4U, .period_cycles = 1U };
    }
    (void) installWatches(watches, 4U);
}

/* ---- fw~conn_trace_001: trace resource configuration ---- */

// [test->fw~conn_trace_001~1]
static void test_trace_init_rejects_bad_resources(void)
{
    static const app_server_region_S zeroLenRegion[] = {
        { .start = TRACE_TEST_BASE, .length = 0U, .base = (uintptr_t) traceMemory },
    };

    app_server_config_S bad = serverConfig;
    bad.readableRegionCount = 0U;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.readableRegions = zeroLenRegion;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.writableRegions = zeroLenRegion;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.watchCapacity = 0U;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.sampleRamBudgetBytes = 0U;
    TEST_ASSERT_FALSE(app_server_init(&bad));

    bad = serverConfig;
    bad.linkBudgetBytesPerS = 0U;
    TEST_ASSERT_FALSE(app_server_init(&bad));
}

/* ---- fw~conn_trace_002: watch-list admission ---- */

// [test->fw~conn_trace_002~1]
// [test->fw~conn_trace_006~1]
static void test_watch_admission_accepted_reports_usage(void)
{
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,       .size = 4U, .period_cycles = 1U   },
        { .address = TRACE_TEST_BASE + 16U, .size = 2U, .period_cycles = 200U },
    };
    const trace_TraceStatus status = installWatches(watches, 2U);
    TEST_ASSERT_EQUAL_UINT32(TRACE_RAM_BUDGET, status.ram_budget_bytes_per_ms);
    // u = 20 x (4 + 4) for the one-cycle group + 1 x (4 + 2) for the 10 ms one
    TEST_ASSERT_EQUAL_UINT32(160U + 6U, status.ram_usage_bytes_per_ms);
    TEST_ASSERT_EQUAL_UINT32(TRACE_LINK_BUDGET, status.link_budget_bytes_per_s);
    // r = (4 B x 20000 Hz + 27 B x 1000 msg/s) + (2 B x 100 Hz + 27 B x 100 msg/s)
    TEST_ASSERT_EQUAL_UINT32(107000U + 2900U, status.link_rate_bytes_per_s);

    // The out-of-band report answers with the same numbers.
    const trace_TraceStatus report = requestTraceStatus(79U);
    TEST_ASSERT_EQUAL_UINT32(status.ram_budget_bytes_per_ms, report.ram_budget_bytes_per_ms);
    TEST_ASSERT_EQUAL_UINT32(status.ram_usage_bytes_per_ms, report.ram_usage_bytes_per_ms);
    TEST_ASSERT_EQUAL_UINT32(status.link_budget_bytes_per_s, report.link_budget_bytes_per_s);
    TEST_ASSERT_EQUAL_UINT32(status.link_rate_bytes_per_s, report.link_rate_bytes_per_s);
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_rejects_bad_entries(void)
{
    const trace_Watch outside = {
        .address = TRACE_TEST_BASE + sizeof(traceMemory) - 2U, .size = 4U, .period_cycles = 1U };
    expectWatchRejection(&outside, 1U, "not readable");

    const trace_Watch sizeZero = { .address = TRACE_TEST_BASE, .size = 0U, .period_cycles = 1U };
    expectWatchRejection(&sizeZero, 1U, "size");

    const trace_Watch sizeBig = { .address = TRACE_TEST_BASE, .size = 9U, .period_cycles = 1U };
    expectWatchRejection(&sizeBig, 1U, "size");

    const trace_Watch noPeriod = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 0U };
    expectWatchRejection(&noPeriod, 1U, "period");

    const trace_Watch oddPeriod = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 300U };
    expectWatchRejection(&oddPeriod, 1U, "period");

    trace_Watch tooMany[TRACE_WATCH_CAPACITY + 1U];
    for (uint32_t i = 0U; i < (TRACE_WATCH_CAPACITY + 1U); i++)
    {
        tooMany[i] = (trace_Watch){ .address = TRACE_TEST_BASE, .size = 1U, .period_cycles = 200U };
    }
    expectWatchRejection(tooMany, TRACE_WATCH_CAPACITY + 1U, "list exceeds");
}

// [test->fw~conn_trace_002~1] nothing caps the one-cycle group's entry count -
// only the RAM and link budgets decide.
static void test_watch_admission_five_one_cycle_entries_accepted(void)
{
    trace_Watch watches[5];
    for (uint32_t i = 0U; i < 5U; i++)
    {
        watches[i] = (trace_Watch){
            .address = TRACE_TEST_BASE + (i * 4U), .size = 4U, .period_cycles = 1U };
    }
    serverConfig.sampleStorage        = traceBigSampleStorage;
    serverConfig.sampleRamBudgetBytes = TRACE_BIG_RAM_BUDGET;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    const trace_TraceStatus status = installWatches(watches, 5U);
    // u = 20 x (4 + 20) = 480, inside the 1024 B/ms budget
    TEST_ASSERT_EQUAL_UINT32(480U, status.ram_usage_bytes_per_ms);
    // r = 20 x 20000 + 27 x m, m = min(20000, max(1000, 20000 x 20 / 256)) = 1562
    TEST_ASSERT_EQUAL_UINT32(400000U + 42174U, status.link_rate_bytes_per_s);
    TEST_ASSERT_EQUAL_UINT32(TRACE_LINK_BUDGET, status.link_budget_bytes_per_s);
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_link_budget_boundary(void)
{
    // One 4-byte 1 ms watch: r = 4 x 1000 + 27 x 1000 = 31000 exactly.
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 20U };

    serverConfig.linkBudgetBytesPerS = 31000U;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    (void) installWatches(&w, 1U);   // asserts acceptance at r == budget

    serverConfig.linkBudgetBytesPerS = 30999U;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    expectWatchRejection(&w, 1U, "link");
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_ram_budget_rejection(void)
{
    serverConfig.sampleRamBudgetBytes = 9U;   // u = 1 x (4 + 8) = 12 > 9
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 8U, .period_cycles = 200U };
    expectWatchRejection(&w, 1U, "RAM");
}

// [test->fw~conn_trace_002~1] a list admitted at exactly u == budget must also
// BUFFER a whole millisecond of records - the ring's headers and empty slot
// live outside the budget, not inside it.
// [test->fw~conn_trace_009~1]
static void test_watch_admission_ram_budget_boundary_fits(void)
{
    serverConfig.sampleRamBudgetBytes = 720U;   // u = 20 x (4 + 32) == budget
    // r = 32 x 20000 + 27 x m, m = min(20000, max(1000, 20000 x 32 / 256)) = 2500
    serverConfig.linkBudgetBytesPerS  = 707500U; // == r, so only the RAM point is on trial
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    trace_Watch watches[4];
    for (uint32_t i = 0U; i < 4U; i++)
    {
        watches[i] = (trace_Watch){
            .address = TRACE_TEST_BASE + (i * 8U), .size = 8U, .period_cycles = 1U };
    }
    (void) installWatches(watches, 4U);   // asserts acceptance at u == budget

    for (uint32_t c = 0U; c < 20U; c++)
    {
        app_server_sampleCycle();   // a full millisecond of worst-case records
    }
    app_server_run1ms();

    // 32-byte records: eight fill a 256-byte Samples, so 20 leave as 8 + 8 + 4.
    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(3U, collectReplies(replies, 8U));
    const uint32_t expectedCounts[] = { 8U, 8U, 4U };
    uint32_t expectedFirst = 0U;
    for (uint32_t m = 0U; m < 3U; m++)
    {
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[m].which_payload);
        TEST_ASSERT_EQUAL_UINT32(1U, replies[m].payload.samples.period_cycles);
        TEST_ASSERT_EQUAL_UINT32(expectedFirst, replies[m].payload.samples.first_cycle);
        TEST_ASSERT_EQUAL_UINT32(expectedCounts[m], replies[m].payload.samples.count);
        TEST_ASSERT_EQUAL_UINT32(expectedCounts[m] * 32U, replies[m].payload.samples.data.size);
        expectedFirst += expectedCounts[m];
    }
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_samples_capacity_rejection(void)
{
    useBigTraceConfig();
    trace_Watch watches[33];
    for (uint32_t i = 0U; i < 33U; i++)
    {
        watches[i] = (trace_Watch){ .address = TRACE_TEST_BASE, .size = 8U, .period_cycles = 200U };
    }
    // 33 x 8 = 264 data bytes > the 256-byte Samples capacity
    expectWatchRejection(watches, 33U, "Samples");
}

// [test->fw~conn_trace_002~1]
static void test_rejected_request_leaves_prior_list_streaming(void)
{
    traceMemory[0] = 0xAABBCCDDU;
    const trace_Watch good = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&good, 1U);

    app_server_sampleCycle();   // cycle 0 buffered

    const trace_Watch bad = { .address = TRACE_TEST_BASE, .size = 0U, .period_cycles = 1U };
    injectWatchRequest(31U, &bad, 1U);
    app_server_run1ms();      // pump rejects, then the drain emits cycle 0

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(2U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[1].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[1].payload.samples.first_cycle);

    app_server_sampleCycle();   // the prior list is still live
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.first_cycle);
}

/* ---- fw~conn_trace_003: watch-list clear on disconnect ---- */

// [test->fw~conn_trace_003~1]
static void test_watch_list_clears_on_disconnect(void)
{
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&w, 1U);
    app_server_sampleCycle();

    HW_USB_sim_setConnected(false);
    app_server_run1ms();   // disconnect edge: list + buffered records die
    HW_USB_sim_setConnected(true);

    app_server_sampleCycle();
    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 41U;
    req.which_payload = shared_Envelope_trace_status_request_tag;
    injectEnvelope(&req);
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_trace_status_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.trace_status.ram_usage_bytes_per_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.trace_status.link_rate_bytes_per_s);
}

// [test->fw~conn_trace_003~1] frames of a dead session are never served: a
// held frame and queued partial bytes both die on the disconnect edge.
static void test_stale_rx_dropped_on_disconnect(void)
{
    app_server_run1ms();   // a quiet connected pass arms the disconnect edge

    // Park a complete valid frame as the channel's held frame (pumped but
    // never received), and queue a partial frame behind it.
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 90U;
    req.which_payload = shared_Envelope_ping_tag;
    injectEnvelope(&req);
    IO_COBSFrame_run();

    const uint8_t partial[] = { 0x00U, 0x11U, 0x22U, 0x33U };
    HW_USB_sim_injectRx(partial, (uint32_t) sizeof(partial));

    HW_USB_sim_setConnected(false);
    app_server_run1ms();   // disconnect edge: framing reset + RX drain
    HW_USB_sim_setConnected(true);

    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));

    req.request_id = 91U;  // the new session's own request is served
    injectEnvelope(&req);
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(91U, replies[0].request_id);
}

/* ---- fw~conn_trace_004: sampling ---- */

// Emit once and flatten every Samples message of `period` into the cycle
// indices of its records, appended to `out`.
static uint32_t drainGroupCycles(uint32_t period, uint32_t * const out, uint32_t maxOut)
{
    app_server_run1ms();
    shared_Envelope replies[32];
    const uint32_t replyCount = collectReplies(replies, 32U);
    uint32_t count = 0U;
    for (uint32_t r = 0U; r < replyCount; r++)
    {
        const trace_Samples * const samples = &replies[r].payload.samples;
        if ((replies[r].which_payload == shared_Envelope_samples_tag) &&
            (samples->period_cycles == period))
        {
            for (uint32_t k = 0U; (k < samples->count) && (count < maxOut); k++)
            {
                out[count] = samples->first_cycle + (k * period);
                count++;
            }
        }
    }
    return count;
}

// Run `cycles` PWM cycles at the real cadence - one emission per 20 of them -
// collecting the cycle indices the group of `period` reported.
static uint32_t runCycles(uint32_t cycles, uint32_t period, uint32_t * const out, uint32_t maxOut)
{
    uint32_t count = 0U;
    for (uint32_t c = 0U; c < cycles; c++)
    {
        app_server_sampleCycle();
        if (((c + 1U) % 20U) == 0U)
        {
            count += drainGroupCycles(period, &out[count], maxOut - count);
        }
    }
    return count;
}

// [test->fw~conn_trace_004~1]
// [test->sys~obs_005~1]
static void test_group_offsets_and_periods(void)
{
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,      .size = 1U, .period_cycles = 1U   },
        { .address = TRACE_TEST_BASE + 4U, .size = 1U, .period_cycles = 20U  },
        { .address = TRACE_TEST_BASE + 8U, .size = 1U, .period_cycles = 200U },
    };
    (void) installWatches(watches, 3U);

    uint32_t fast[512];
    const uint32_t fastCount = runCycles(420U, 1U, fast, (uint32_t) COUNTOF(fast));
    TEST_ASSERT_EQUAL_UINT32(420U, fastCount);
    for (uint32_t i = 0U; i < fastCount; i++)
    {
        TEST_ASSERT_EQUAL_UINT32(i, fast[i]);   // offset 0, every cycle
    }

    (void) installWatches(watches, 3U);
    uint32_t medium[64];
    const uint32_t mediumCount = runCycles(420U, 20U, medium, (uint32_t) COUNTOF(medium));
    TEST_ASSERT_EQUAL_UINT32(21U, mediumCount);
    for (uint32_t i = 0U; i < mediumCount; i++)
    {
        TEST_ASSERT_EQUAL_UINT32(1U + (i * 20U), medium[i]);   // offset 1, every 20
    }

    (void) installWatches(watches, 3U);
    uint32_t slow[8];
    const uint32_t slowCount = runCycles(420U, 200U, slow, (uint32_t) COUNTOF(slow));
    TEST_ASSERT_EQUAL_UINT32(3U, slowCount);
    TEST_ASSERT_EQUAL_UINT32(2U, slow[0]);       // offset 2, every 200
    TEST_ASSERT_EQUAL_UINT32(202U, slow[1]);
    TEST_ASSERT_EQUAL_UINT32(402U, slow[2]);
}

// [test->fw~conn_trace_009~1] capture order across groups, with each group's
// consecutive run sharing one message
static void test_capture_order_and_batching(void)
{
    traceMemory[0] = 0x11223344U;
    traceMemory[1] = 0x0000BEEFU;
    traceMemory[2] = 0x000000A5U;
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,      .size = 4U, .period_cycles = 1U   },
        { .address = TRACE_TEST_BASE + 4U, .size = 2U, .period_cycles = 20U  },
        { .address = TRACE_TEST_BASE + 8U, .size = 1U, .period_cycles = 200U },
    };
    (void) installWatches(watches, 3U);

    for (uint32_t c = 0U; c < 20U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();

    // Capture order is 1@0 | 1@1, 20@1 | 1@2, 200@2 | 1@3..19, so each group's
    // run breaks exactly where another group's record lands between them.
    const uint32_t expected[5][4] = {
        //  period, first_cycle, count, data bytes
        {   1U,  0U,  2U,  8U },
        {  20U,  1U,  1U,  2U },
        {   1U,  2U,  1U,  4U },
        { 200U,  2U,  1U,  1U },
        {   1U,  3U, 17U, 68U },
    };
    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(5U, collectReplies(replies, 8U));
    for (uint32_t m = 0U; m < 5U; m++)
    {
        const trace_Samples * const samples = &replies[m].payload.samples;
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[m].which_payload);
        TEST_ASSERT_EQUAL_UINT32(expected[m][0], samples->period_cycles);
        TEST_ASSERT_EQUAL_UINT32(expected[m][1], samples->first_cycle);
        TEST_ASSERT_EQUAL_UINT32(expected[m][2], samples->count);
        TEST_ASSERT_EQUAL_UINT32(expected[m][3], samples->data.size);
    }
    TEST_ASSERT_EQUAL_UINT8(0x44U, replies[0].payload.samples.data.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x11U, replies[0].payload.samples.data.bytes[3]);
    TEST_ASSERT_EQUAL_UINT8(0xEFU, replies[1].payload.samples.data.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBEU, replies[1].payload.samples.data.bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(0xA5U, replies[3].payload.samples.data.bytes[0]);
}

// [test->fw~conn_trace_009~1] a 10 ms group buffers at most one record per
// emission, so its records each ride their own message
static void test_slow_group_records_ride_their_own_message(void)
{
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 200U };
    (void) installWatches(&w, 1U);

    uint32_t messages = 0U;
    for (uint32_t c = 0U; c < 420U; c++)
    {
        app_server_sampleCycle();
        if (((c + 1U) % 20U) == 0U)
        {
            app_server_run1ms();
            shared_Envelope replies[4];
            const uint32_t replyCount = collectReplies(replies, 4U);
            for (uint32_t r = 0U; r < replyCount; r++)
            {
                TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
                TEST_ASSERT_EQUAL_UINT32(1U, replies[r].payload.samples.count);
                messages++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3U, messages);
}

// [test->fw~conn_trace_004~1] the sampler's gate: with no list installed the
// cycle index never advances, so an install always starts the stream at zero.
static void test_sampler_quiet_without_a_list(void)
{
    for (uint32_t c = 0U; c < 50U; c++)
    {
        app_server_sampleCycle();
    }
    shared_Envelope replies[4];
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 4U));

    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&w, 1U);
    app_server_sampleCycle();
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.samples.first_cycle);
}

// [test->fw~conn_trace_004~1]
static void test_new_list_restarts_stream_discarding_buffered(void)
{
    const trace_Watch first = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&first, 1U);
    for (uint32_t c = 0U; c < 3U; c++)
    {
        app_server_sampleCycle();   // cycles 0..2 buffered, never drained
    }

    const trace_Watch second = { .address = TRACE_TEST_BASE + 4U, .size = 2U, .period_cycles = 20U };
    // installWatches asserts exactly one reply: the buffered prior-list records
    // were discarded by the install, not drained after it.
    (void) installWatches(&second, 1U);

    for (uint32_t c = 0U; c < 2U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(20U, replies[0].payload.samples.period_cycles);
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.first_cycle);   // the new group's offset
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(2U, replies[0].payload.samples.data.size);
}

// [test->fw~conn_trace_004~1]
static void test_ring_overflow_skips_whole_records_leaving_gap(void)
{
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&w, 1U);

    // Record = 11 B, ring free = 322 B (budget 256 + overhead 67 - empty slot):
    // cycles 0..28 fit, 29..34 are skipped.
    for (uint32_t c = 0U; c < 35U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(29U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(29U * 4U, replies[0].payload.samples.data.size);

    app_server_sampleCycle();   // the cycle index jumps the gap
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL_UINT32(35U, replies[0].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.count);
}

// [test->fw~conn_trace_004~1] two locations the firmware updates together
// between cycles arrive mutually consistent in every capture
// [test->sys~obs_005~1]
static void test_one_cycle_group_captures_a_coherent_snapshot(void)
{
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,      .size = 4U, .period_cycles = 1U },
        { .address = TRACE_TEST_BASE + 4U, .size = 4U, .period_cycles = 1U },
    };
    (void) installWatches(watches, 2U);

    uint32_t records = 0U;
    for (uint32_t c = 0U; c < 60U; c++)
    {
        traceMemory[0] = c + 1U;    // the pair a firmware writer updates together
        traceMemory[1] = ~(c + 1U);
        app_server_sampleCycle();
        if (((c + 1U) % 20U) == 0U)
        {
            app_server_run1ms();
            shared_Envelope replies[16];
            const uint32_t n = collectReplies(replies, 16U);
            for (uint32_t r = 0U; r < n; r++)
            {
                const trace_Samples * const samples = &replies[r].payload.samples;
                TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
                TEST_ASSERT_EQUAL_UINT32(samples->count * 8U, samples->data.size);
                for (uint32_t k = 0U; k < samples->count; k++)
                {
                    uint32_t first  = 0U;
                    uint32_t second = 0U;
                    (void) memcpy(&first, &samples->data.bytes[k * 8U], 4U);
                    (void) memcpy(&second, &samples->data.bytes[(k * 8U) + 4U], 4U);
                    TEST_ASSERT_EQUAL_UINT32(~first, second);
                    records++;
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(60U, records);
}

// [test->fw~conn_trace_004~1] a run broken in the middle by dropped records
// never shares a message: the batch ends where the cycle index stops stepping
// [test->fw~conn_trace_009~1]
static void test_interior_gap_splits_the_batch(void)
{
    useDeepTraceConfig();
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_cycles = 1U };
    (void) installWatches(&w, 1U);

    // Stall emission until the ring is full and dropping, then let one pass
    // take as much as the 2 KB transport holds - the rest stays buffered.
    HW_USB_sim_setTxAccepting(false);
    for (uint32_t c = 0U; c < 600U; c++)
    {
        app_server_sampleCycle();
    }
    HW_USB_sim_setTxAccepting(true);
    app_server_run1ms();

    for (uint32_t c = 0U; c < 10U; c++)
    {
        app_server_sampleCycle();   // a second run, past the dropped middle
    }

    shared_Envelope replies[16];
    (void) collectReplies(replies, 16U);   // discard the partial drain
    app_server_run1ms();
    const uint32_t n = collectReplies(replies, 16U);
    TEST_ASSERT_TRUE(n >= 2U);

    uint32_t gaps = 0U;
    for (uint32_t r = 0U; r < n; r++)
    {
        const trace_Samples * const samples = &replies[r].payload.samples;
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
        TEST_ASSERT_EQUAL_UINT32(1U, samples->period_cycles);
        TEST_ASSERT_EQUAL_UINT32(samples->count * 4U, samples->data.size);
        if (r > 0U)
        {
            const trace_Samples * const prior = &replies[r - 1U].payload.samples;
            if (samples->first_cycle != (prior->first_cycle + prior->count))
            {
                gaps++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1U, gaps);

    const trace_Samples * const last = &replies[n - 1U].payload.samples;
    TEST_ASSERT_EQUAL_UINT32(600U, last->first_cycle);
    TEST_ASSERT_EQUAL_UINT32(10U, last->count);
}

// [test->fw~conn_trace_004~1] a one-cycle group flooding the ring never
// corrupts the staggered 20-cycle group: its records stay whole, on its
// offset, and no emitted index runs past what was sampled
// [test->fw~conn_trace_009~1]
static void test_overflow_keeps_the_slow_group_records_honest(void)
{
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,       .size = 4U, .period_cycles = 1U  },
        { .address = TRACE_TEST_BASE + 16U, .size = 8U, .period_cycles = 20U },
    };
    (void) installWatches(watches, 2U);

    for (uint32_t c = 0U; c < 100U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();

    shared_Envelope replies[16];
    const uint32_t n = collectReplies(replies, 16U);
    TEST_ASSERT_TRUE(n >= 2U);

    uint32_t slowRecords = 0U;
    uint32_t lastFastEnd = 0U;
    bool     haveFast    = false;
    for (uint32_t r = 0U; r < n; r++)
    {
        const trace_Samples * const samples = &replies[r].payload.samples;
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
        TEST_ASSERT_TRUE(samples->count > 0U);
        TEST_ASSERT_TRUE((samples->first_cycle +
                          ((samples->count - 1U) * samples->period_cycles)) < 100U);
        if (samples->period_cycles == 20U)
        {
            TEST_ASSERT_EQUAL_UINT32(samples->count * 8U, samples->data.size);
            TEST_ASSERT_EQUAL_UINT32(1U, samples->first_cycle % 20U);
            slowRecords += samples->count;
        }
        else
        {
            TEST_ASSERT_EQUAL_UINT32(1U, samples->period_cycles);
            TEST_ASSERT_EQUAL_UINT32(samples->count * 4U, samples->data.size);
            if (haveFast)
            {
                TEST_ASSERT_TRUE(samples->first_cycle > lastFastEnd);
            }
            lastFastEnd = samples->first_cycle + samples->count - 1U;
            haveFast = true;
        }
    }
    TEST_ASSERT_TRUE(haveFast);
    // Ring usable space is (256 budget + 67 overhead) - 1 = 322 B. A fast
    // record costs 3 + 4 + 4 = 11 B, a slow one 3 + 4 + 8 = 15 B; cycles 0..25
    // admit 26 fast and the slow records of cycles 1 and 21 (26 x 11 + 2 x 15 =
    // 316 B) before cycle 26's fast record no longer fits.
    TEST_ASSERT_EQUAL_UINT32(2U, slowRecords);
}

// [test->fw~conn_trace_004~1] once a record is skipped, admission waits for
// the buffer to drain to half, so the loss reads as one contiguous gap
static void test_overflow_holds_until_the_buffer_drains_to_half(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    // Fill the buffer past overflow with nothing emitted, then let one pass
    // drain a few records into a transport with room for one message only.
    for (uint32_t c = 0U; c < 400U; c++)
    {
        app_server_sampleCycle();
    }
    packTxTo(TRACE_ONE_MESSAGE_FREE);
    app_server_run1ms();
    shared_Envelope replies[64];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 64U));
    TEST_ASSERT_EQUAL_UINT32(16U, replies[0].payload.samples.count);

    // Room for 16 records now, but the buffer is still above half: these
    // cycles are skipped, not admitted one-for-one into the freed space.
    for (uint32_t c = 400U; c < 420U; c++)
    {
        app_server_sampleCycle();
    }
    // Drain everything that was buffered, then sample a fresh run.
    uint32_t total = 0U;
    uint32_t lastEnd = 0U;
    for (uint32_t pass = 0U; pass < 8U; pass++)
    {
        app_server_run1ms();
        const uint32_t n = collectReplies(replies, 64U);
        for (uint32_t i = 0U; i < n; i++)
        {
            const trace_Samples * const m = &replies[i].payload.samples;
            total += m->count;
            lastEnd = m->first_cycle + m->count - 1U;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(lastEnd < 400U, "a record from the held-off span was admitted");
    for (uint32_t c = 420U; c < 440U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    const uint32_t n = collectReplies(replies, 64U);
    TEST_ASSERT_TRUE(n >= 1U);
    TEST_ASSERT_EQUAL_UINT32(420U, replies[0].payload.samples.first_cycle);
    uint32_t fresh = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        fresh += replies[i].payload.samples.count;
    }
    TEST_ASSERT_EQUAL_UINT32(20U, fresh);
    // Record conservation: a 16-byte record costs 3 + 4 + 16 = 23 B and the
    // ring's usable space is (6144 budget + 67 overhead) - 1 = 6210 B, so
    // cycles 0..269 fill it exactly and cycle 270 overflows. 16 of those 270
    // left in the first pass; the rest drain here.
    TEST_ASSERT_EQUAL_UINT32(270U - 16U, total);
}

// [test->fw~conn_trace_009~1] 16-byte records fill a 256-byte Samples at 16 of
// them, so a millisecond of one-cycle captures leaves as 16 + 4
static void test_sixteen_byte_records_split_a_millisecond_as_sixteen_and_four(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    for (uint32_t c = 0U; c < 20U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();

    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(2U, collectReplies(replies, 8U));
    const uint32_t expectedCounts[] = { 16U, 4U };
    uint32_t expectedFirst = 0U;
    for (uint32_t m = 0U; m < 2U; m++)
    {
        const trace_Samples * const samples = &replies[m].payload.samples;
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[m].which_payload);
        TEST_ASSERT_EQUAL_UINT32(1U, samples->period_cycles);
        TEST_ASSERT_EQUAL_UINT32(expectedFirst, samples->first_cycle);
        TEST_ASSERT_EQUAL_UINT32(expectedCounts[m], samples->count);
        TEST_ASSERT_EQUAL_UINT32(expectedCounts[m] * 16U, samples->data.size);
        expectedFirst += expectedCounts[m];
    }
}

// [test->fw~conn_trace_009~1] with transmit capacity for less than a full
// message nothing leaves — no fragment either — and the records arrive whole
// once capacity returns
// [test->fw~conn_proto_004~1] a frame the remaining capacity cannot hold
// reaches the transport whole or not at all
static void test_short_transmit_capacity_holds_records_whole(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    // Pack the transport down to the reply reserve plus room for a few
    // records: short of one full 16-record message.
    packTxTo((uint32_t) IO_COBSFRAME_WIRE_MAX(LIB_PROTOBUF_ENVELOPE_MAX) + 96U);
    const uint32_t txLenBefore = HW_USB_sim_txLen();

    for (uint32_t c = 0U; c < 20U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(txLenBefore, HW_USB_sim_txLen());

    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 8U));   // frees the transport
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(2U, collectReplies(replies, 8U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(16U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(16U, replies[1].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(4U, replies[1].payload.samples.count);
}

// [test->fw~conn_trace_009~1] records buffered across a 3 ms emission stall all
// arrive in the next emission, in capture order
static void test_records_stalled_three_milliseconds_arrive_in_capture_order(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    for (uint32_t c = 0U; c < 60U; c++)
    {
        traceMemory[0] = c;         // a per-cycle stamp the emitted order is read from
        app_server_sampleCycle();   // three passes' worth, with no emission
    }

    app_server_run1ms();
    shared_Envelope replies[16];
    const uint32_t n = collectReplies(replies, 16U);

    uint32_t seen = 0U;
    for (uint32_t r = 0U; r < n; r++)
    {
        const trace_Samples * const samples = &replies[r].payload.samples;
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
        TEST_ASSERT_EQUAL_UINT32(seen, samples->first_cycle);
        for (uint32_t k = 0U; k < samples->count; k++)
        {
            uint32_t stamp = 0U;
            (void) memcpy(&stamp, &samples->data.bytes[k * 16U], 4U);
            TEST_ASSERT_EQUAL_UINT32(seen, stamp);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(60U, seen);
}

// [test->fw~conn_trace_009~1] no record is held longer than 2 ms: a record
// captured in pass n leaves no later than pass n + 2
static void test_no_record_is_held_past_two_emissions(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    uint32_t emitted = 0U;
    uint32_t maxLag = 0U;
    for (uint32_t pass = 0U; pass < 12U; pass++)
    {
        for (uint32_t c = 0U; c < 20U; c++)
        {
            app_server_sampleCycle();
        }
        // Every third pass has room for one message only, so 4 of its 20
        // records carry over and the following pass runs against a backlog.
        if ((pass % 3U) == 0U)
        {
            packTxTo(TRACE_ONE_MESSAGE_FREE);
        }
        app_server_run1ms();

        shared_Envelope replies[16];
        const uint32_t n = collectReplies(replies, 16U);
        for (uint32_t r = 0U; r < n; r++)
        {
            const trace_Samples * const samples = &replies[r].payload.samples;
            TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[r].which_payload);
            for (uint32_t k = 0U; k < samples->count; k++)
            {
                const uint32_t capturedInPass = (samples->first_cycle + k) / 20U;
                const uint32_t lag = pass - capturedInPass;
                TEST_ASSERT_TRUE(lag <= 2U);
                maxLag = (lag > maxLag) ? lag : maxLag;
                emitted++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(240U, emitted);
    TEST_ASSERT_TRUE_MESSAGE(maxLag >= 1U, "the packed passes built no backlog");
}

/* ---- fw~conn_trace_005: Samples message format ---- */

// [test->fw~conn_trace_005~1] a known list and record encode to a byte-exact
// reference frame
static void test_samples_frame_is_byte_exact(void)
{
    traceMemory[1] = 0x0000BEEFU;
    const trace_Watch w = { .address = TRACE_TEST_BASE + 4U, .size = 2U, .period_cycles = 20U };
    (void) installWatches(&w, 1U);

    for (uint32_t c = 0U; c < 2U; c++)
    {
        app_server_sampleCycle();   // cycle 1 is the 20-cycle group's offset
    }
    app_server_run1ms();

    // Envelope{ samples = Samples{ first_cycle: 1, data: EF BE,
    //                              period_cycles: 20, count: 1 } }, then the
    // little-endian CRC-32 of those 13 bytes, COBS-encoded (one 0x12 group
    // code, no zero bytes to escape) between two delimiters. Every byte below
    // is a reference value, not a call into the codecs under test.
    const uint8_t expected[] = {
        0x00U,                             // leading delimiter
        0x12U,                             // COBS code: 17 nonzero bytes follow
        0x8AU, 0x02U, 0x0AU,               // field 33, length-delimited, 10 bytes
        0x08U, 0x01U,                      // first_cycle = 1
        0x12U, 0x02U, 0xEFU, 0xBEU,        // data = EF BE
        0x18U, 0x14U,                      // period_cycles = 20
        0x20U, 0x01U,                      // count = 1
        0xAAU, 0xCFU, 0x9CU, 0x12U,        // CRC-32 = 0x129CCFAA, little endian
        0x00U,                             // trailing delimiter
    };

    uint8_t wire[512];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) sizeof(expected), wireLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, wire, wireLen);
}

// [test->fw~conn_trace_005~1]
static void test_max_samples_frame_layout_and_wire_bound(void)
{
    useBigTraceConfig();
    for (uint32_t i = 0U; i < 64U; i++)
    {
        traceMemory[i] = 0x40302010U + i;
    }
    trace_Watch watches[32];
    for (uint32_t i = 0U; i < 32U; i++)
    {
        watches[i] = (trace_Watch){
            .address = TRACE_TEST_BASE + (i * 8U), .size = 8U, .period_cycles = 200U };
    }
    (void) installWatches(watches, 32U);

    for (uint32_t c = 0U; c < 3U; c++)
    {
        app_server_sampleCycle();   // cycle 2: all 32 spans, 256 data bytes
    }
    app_server_run1ms();

    // Whole wire frame within data + W: 256 + 27. This frame spends 1 byte on
    // first_cycle where W budgets 5, so it sits a handful under the bound.
    uint8_t wire[2048];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    TEST_ASSERT_TRUE(wireLen > 256U);
    TEST_ASSERT_TRUE(wireLen <= 283U);
    TEST_ASSERT_TRUE_MESSAGE((wireLen + 20U) > 283U, "W overstates the frame by more than 20 B");

    // And the payload is the watched spans concatenated in list order.
    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(200U, replies[0].payload.samples.period_cycles);
    TEST_ASSERT_EQUAL_UINT32(2U, replies[0].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(256U, replies[0].payload.samples.data.size);
    for (uint32_t i = 0U; i < 32U; i++)
    {
        TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *) &traceMemory[i * 2U],
                                      &replies[0].payload.samples.data.bytes[i * 8U], 8U);
    }
}

// [test->fw~conn_trace_005~1] a full 16-record message past cycle 128 — where
// first_cycle costs a two-byte varint — still fits W
static void test_full_message_with_a_multi_byte_first_cycle_fits_the_bound(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    // Seven drained milliseconds put the cycle index at 140, then 16 cycles
    // fill one message exactly: 16 x 16 B of data at first_cycle 140.
    shared_Envelope replies[8];
    for (uint32_t pass = 0U; pass < 7U; pass++)
    {
        for (uint32_t c = 0U; c < 20U; c++)
        {
            app_server_sampleCycle();
        }
        app_server_run1ms();
        (void) collectReplies(replies, 8U);
    }
    for (uint32_t c = 0U; c < 16U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();

    uint8_t wire[512];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    TEST_ASSERT_TRUE(wireLen > 256U);
    TEST_ASSERT_TRUE(wireLen <= 283U);
    TEST_ASSERT_TRUE_MESSAGE((wireLen + 20U) > 283U, "W overstates the frame by more than 20 B");

    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 8U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.samples.first_cycle >= 128U);
    TEST_ASSERT_EQUAL_UINT32(140U, replies[0].payload.samples.first_cycle);
    TEST_ASSERT_EQUAL_UINT32(16U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(256U, replies[0].payload.samples.data.size);
}

/* ---- fw~conn_trace_007/008: one-shot read and write ---- */

// [test->fw~conn_trace_007~1]
// [test->sys~obs_008~1]
static void test_read_returns_current_contents(void)
{
    traceMemory[1] = 0xA1B2C3D4U;
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 51U;
    req.which_payload = shared_Envelope_read_request_tag;
    req.payload.read_request.address = TRACE_TEST_BASE + 4U;
    req.payload.read_request.size = 4U;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_read_reply_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(51U, replies[0].request_id);
    TEST_ASSERT_EQUAL_UINT32(4U, replies[0].payload.read_reply.data.size);
    TEST_ASSERT_EQUAL_UINT8(0xD4U, replies[0].payload.read_reply.data.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA1U, replies[0].payload.read_reply.data.bytes[3]);
}

static void expectReadRejection(uint32_t address, uint32_t size, const char * const causeSubstring)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 52U;
    req.which_payload = shared_Envelope_read_request_tag;
    req.payload.read_request.address = address;
    req.payload.read_request.size = size;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, causeSubstring));
}

// [test->fw~conn_trace_007~1]
// [test->sys~obs_008~1]
static void test_read_rejections(void)
{
    expectReadRejection(TRACE_TEST_BASE, 0U, "size");
    expectReadRejection(TRACE_TEST_BASE, 129U, "size");
    expectReadRejection(TRACE_TEST_BASE + sizeof(traceMemory) - 2U, 4U, "not readable");
}

// [test->fw~conn_trace_008~1]
static void test_write_lands_and_reads_back(void)
{
    traceMemory[2] = 0U;
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 61U;
    req.which_payload = shared_Envelope_write_request_tag;
    req.payload.write_request.address = TRACE_TEST_BASE + 8U;
    req.payload.write_request.data.size = 4U;
    req.payload.write_request.data.bytes[0] = 0x01U;
    req.payload.write_request.data.bytes[1] = 0x02U;
    req.payload.write_request.data.bytes[2] = 0x03U;
    req.payload.write_request.data.bytes[3] = 0x04U;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0x04030201U, traceMemory[2]);

    // The copy lands inside one mask/unmask pair, so the cycle sampler - which
    // outranks the FreeRTOS critical section - never reads a torn span.
    TEST_ASSERT_EQUAL_UINT32(2U, samplerMaskCalls);
    TEST_ASSERT_FALSE(samplerMasked);
    TEST_ASSERT_EQUAL_UINT32(0U, traceWordWhileMasked);
    TEST_ASSERT_EQUAL_UINT32(0x04030201U, traceWordWhileUnmasked);
}

// [test->fw~conn_trace_008~1] the sampler-mask hook is optional: a board that
// supplies none still takes the write.
static void test_write_without_sampler_mask_hook_lands(void)
{
    traceMemory[2] = 0U;
    serverConfig.setSamplerMasked = NULL;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 63U;
    req.which_payload = shared_Envelope_write_request_tag;
    req.payload.write_request.address = TRACE_TEST_BASE + 8U;
    req.payload.write_request.data.size = 4U;
    req.payload.write_request.data.bytes[0] = 0x01U;
    req.payload.write_request.data.bytes[1] = 0x02U;
    req.payload.write_request.data.bytes[2] = 0x03U;
    req.payload.write_request.data.bytes[3] = 0x04U;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL_UINT32(0x04030201U, traceMemory[2]);
    TEST_ASSERT_EQUAL_UINT32(0U, samplerMaskCalls);
}

static void expectWriteRejection(uint32_t address, uint32_t len, const char * const causeSubstring)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 62U;
    req.which_payload = shared_Envelope_write_request_tag;
    req.payload.write_request.address = address;
    req.payload.write_request.data.size = (pb_size_t) len;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, causeSubstring));
}

// [test->fw~conn_trace_008~1]
static void test_write_rejections(void)
{
    expectWriteRejection(TRACE_TEST_BASE, 0U, "size");
    // Readable back half, outside the writable front half.
    expectWriteRejection(TRACE_TEST_BASE + 200U, 4U, "not writable");
}

/* ---- fw~conn_server_005: link throughput test ---- */

static void injectLinkTestRequest(uint32_t requestId, uint32_t payloadBytes, uint32_t frameCount)
{
    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = requestId;
    req.which_payload = shared_Envelope_link_test_request_tag;
    req.payload.link_test_request.payload_bytes = payloadBytes;
    req.payload.link_test_request.frame_count = frameCount;
    injectEnvelope(&req);
}

// Run maxPasses server passes, checking every streamed frame's seq and pattern
// payload against the running count; returns the frames seen.
static uint32_t drainLinkTestFrames(uint32_t payloadBytes, uint32_t maxPasses)
{
    uint32_t frames = 0U;
    for (uint32_t pass = 0U; pass < maxPasses; pass++)
    {
        app_server_run1ms();
        shared_Envelope replies[32];
        const uint32_t n = collectReplies(replies, 32U);
        for (uint32_t r = 0U; r < n; r++)
        {
            TEST_ASSERT_EQUAL(shared_Envelope_link_test_frame_tag, replies[r].which_payload);
            TEST_ASSERT_EQUAL_UINT32(0U, replies[r].request_id);   // a stream, like Samples
            TEST_ASSERT_EQUAL_UINT32(frames, replies[r].payload.link_test_frame.seq);
            TEST_ASSERT_EQUAL_size_t(payloadBytes, replies[r].payload.link_test_frame.payload.size);
            for (uint32_t i = 0U; i < payloadBytes; i++)
            {
                TEST_ASSERT_EQUAL_UINT8((uint8_t) ((frames + i) & 0xFFU),
                                        replies[r].payload.link_test_frame.payload.bytes[i]);
            }
            frames++;
        }
    }
    return frames;
}

// The accepted request answers with a Response, then the stream follows in the
// same pass; returns the frames that rode along with the verdict.
static uint32_t acceptLinkTest(uint32_t requestId, uint32_t payloadBytes, uint32_t frameCount)
{
    injectLinkTestRequest(requestId, payloadBytes, frameCount);
    app_server_run1ms();

    shared_Envelope replies[32];
    const uint32_t n = collectReplies(replies, 32U);
    TEST_ASSERT_TRUE(n >= 1U);
    TEST_ASSERT_EQUAL_UINT32(requestId, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);

    for (uint32_t r = 1U; r < n; r++)
    {
        const uint32_t seq = r - 1U;
        TEST_ASSERT_EQUAL(shared_Envelope_link_test_frame_tag, replies[r].which_payload);
        TEST_ASSERT_EQUAL_UINT32(seq, replies[r].payload.link_test_frame.seq);
        TEST_ASSERT_EQUAL_size_t(payloadBytes, replies[r].payload.link_test_frame.payload.size);
        for (uint32_t i = 0U; i < payloadBytes; i++)
        {
            TEST_ASSERT_EQUAL_UINT8((uint8_t) ((seq + i) & 0xFFU),
                                    replies[r].payload.link_test_frame.payload.bytes[i]);
        }
    }
    return n - 1U;
}

// [test->fw~conn_server_005~1]
static void test_link_test_streams_exactly_the_requested_frames(void)
{
    const uint32_t payloadBytes = 64U;
    const uint32_t frameCount = 40U;

    uint32_t frames = acceptLinkTest(70U, payloadBytes, frameCount);
    TEST_ASSERT_TRUE(frames < frameCount);   // the 2 KB sim transport fills first

    // The rest streams over later passes as room frees, then stops dead.
    uint32_t seen = 0U;
    for (uint32_t pass = 0U; (pass < 8U) && (frames + seen < frameCount); pass++)
    {
        app_server_run1ms();
        shared_Envelope replies[32];
        const uint32_t n = collectReplies(replies, 32U);
        for (uint32_t r = 0U; r < n; r++)
        {
            const uint32_t seq = frames + seen;
            TEST_ASSERT_EQUAL(shared_Envelope_link_test_frame_tag, replies[r].which_payload);
            TEST_ASSERT_EQUAL_UINT32(0U, replies[r].request_id);
            TEST_ASSERT_EQUAL_UINT32(seq, replies[r].payload.link_test_frame.seq);
            TEST_ASSERT_EQUAL_size_t(payloadBytes, replies[r].payload.link_test_frame.payload.size);
            for (uint32_t i = 0U; i < payloadBytes; i++)
            {
                TEST_ASSERT_EQUAL_UINT8((uint8_t) ((seq + i) & 0xFFU),
                                        replies[r].payload.link_test_frame.payload.bytes[i]);
            }
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(frameCount, frames + seen);

    TEST_ASSERT_EQUAL_UINT32(0U, drainLinkTestFrames(payloadBytes, 4U));
}

static void expectLinkTestRejection(uint32_t payloadBytes, uint32_t frameCount,
                                    const char * const causeSubstring)
{
    injectLinkTestRequest(71U, payloadBytes, frameCount);
    app_server_run1ms();

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));   // verdict only, no stream
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, causeSubstring));
}

// [test->fw~conn_server_005~1]
static void test_link_test_rejects_out_of_range_requests(void)
{
    expectLinkTestRejection(0U, 10U, "payload_bytes");
    expectLinkTestRejection(257U, 10U, "payload_bytes");
    expectLinkTestRejection(64U, 0U, "frame_count");
    expectLinkTestRejection(64U, 1000001U, "frame_count");
}

// [test->fw~conn_server_005~1]
static void test_link_test_rejects_second_request_while_running(void)
{
    // A 256-byte payload fills the sim transport long before 200 frames, so
    // the test is still running when the second request lands.
    (void) acceptLinkTest(72U, 256U, 200U);

    injectLinkTestRequest(73U, 8U, 1U);
    app_server_run1ms();

    shared_Envelope replies[32];
    const uint32_t n = collectReplies(replies, 32U);
    TEST_ASSERT_TRUE(n >= 1U);
    TEST_ASSERT_EQUAL_UINT32(73U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_NOT_NULL(strstr(replies[0].payload.response.cause, "already running"));

    // The running test keeps its own sequence — the rejection changed nothing.
    for (uint32_t r = 1U; r < n; r++)
    {
        TEST_ASSERT_EQUAL(shared_Envelope_link_test_frame_tag, replies[r].which_payload);
    }
}

// [test->fw~conn_server_005~1]
static void test_link_test_count_reached_frees_the_service(void)
{
    TEST_ASSERT_EQUAL_UINT32(3U, acceptLinkTest(74U, 16U, 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, drainLinkTestFrames(16U, 2U));

    // Seq restarts at zero: a new test, not a continuation of the last one.
    TEST_ASSERT_EQUAL_UINT32(2U, acceptLinkTest(75U, 16U, 2U));
}

// [test->fw~conn_server_005~1] a link test dies with the port: nothing more
// streams after the disconnect, and the next session starts a fresh count
static void test_link_test_clears_on_disconnect(void)
{
    (void) acceptLinkTest(76U, 256U, 200U);   // still running when the port drops

    HW_USB_sim_setConnected(false);
    app_server_run1ms();   // disconnect edge: the test is abandoned with the list
    HW_USB_sim_setConnected(true);

    app_server_run1ms();
    shared_Envelope replies[32];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 32U));

    // A fresh request is accepted, its sequence restarting at zero.
    TEST_ASSERT_EQUAL_UINT32(2U, acceptLinkTest(77U, 16U, 2U));
}


// [test->fw~conn_proto_001~1] an envelope assembled around an encoded payload
// is byte-identical to the envelope encoded whole
static void test_envelope_assembled_around_a_payload_matches_the_whole(void)
{
    shared_Envelope env = shared_Envelope_init_zero;
    env.request_id = 0U;
    env.which_payload = shared_Envelope_samples_tag;
    env.payload.samples.period_cycles = 20U;
    env.payload.samples.first_cycle = 0x12345678U;
    env.payload.samples.count = 3U;
    env.payload.samples.data.size = 12U;
    for (uint32_t i = 0U; i < 12U; i++)
    {
        env.payload.samples.data.bytes[i] = (uint8_t) (0xA0U + i);
    }
    uint8_t whole[64];
    uint8_t assembled[64];
    size_t wholeLen = 0U;
    size_t assembledLen = 0U;
    TEST_ASSERT_TRUE(lib_protobuf_encode(shared_Envelope_fields, &env, whole, sizeof(whole), &wholeLen));
    TEST_ASSERT_TRUE(lib_protobuf_encodeEnvelope(0U, shared_Envelope_samples_tag, trace_Samples_fields,
                                                 &env.payload.samples, assembled, sizeof(assembled), &assembledLen));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) wholeLen, (uint32_t) assembledLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(whole, assembled, wholeLen);

    // With a request id (a two-byte varint) and a reply payload.
    env = (shared_Envelope) shared_Envelope_init_zero;
    env.request_id = 300U;
    env.which_payload = shared_Envelope_response_tag;
    env.payload.response.accepted = true;
    TEST_ASSERT_TRUE(lib_protobuf_encode(shared_Envelope_fields, &env, whole, sizeof(whole), &wholeLen));
    TEST_ASSERT_TRUE(lib_protobuf_encodeEnvelope(300U, shared_Envelope_response_tag, shared_Response_fields,
                                                 &env.payload.response, assembled, sizeof(assembled), &assembledLen));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) wholeLen, (uint32_t) assembledLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(whole, assembled, wholeLen);
    TEST_ASSERT_FALSE(lib_protobuf_encodeEnvelope(0U, shared_Envelope_samples_tag, trace_Samples_fields,
                                                  &env.payload.samples, assembled, 8U, &assembledLen));
}

// [test->fw~conn_server_006~1] a pass's frames reach the transport as one
// write, in order; a frame the pass cannot hold stays out while the earlier
// ones still leave
// [test->fw~conn_proto_004~1]
static void test_a_pass_leaves_as_one_write(void)
{
    useDeepTraceConfig();
    installFourFastWatches();
    resetTx();

    // Phase the divider so the measured pass is the telemetry one: the install
    // took a pass, and the measured pass is the period's last.
    for (uint32_t tick = 0U; tick < (APP_SERVER_TELEMETRY_PERIOD_TICKS - 2U); tick++)
    {
        app_server_run1ms();
    }
    resetTx();
    for (uint32_t c = 0U; c < 20U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, HW_USB_sim_txWrites());
    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(3U, collectReplies(replies, 8U));
    TEST_ASSERT_EQUAL(shared_Envelope_telemetry_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[1].which_payload);
    TEST_ASSERT_EQUAL_UINT32(16U, replies[1].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(4U, replies[2].payload.samples.count);

    // Room for the reply reserve plus one full message only: the 16-record
    // frame leaves, the 4-record one waits for the next pass.
    packTxTo(TRACE_ONE_MESSAGE_FREE);
    for (uint32_t c = 20U; c < 40U; c++)
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 8U));
    TEST_ASSERT_EQUAL_UINT32(16U, replies[0].payload.samples.count);
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 8U));
    TEST_ASSERT_EQUAL_UINT32(4U, replies[0].payload.samples.count);
    TEST_ASSERT_EQUAL_UINT32(36U, replies[0].payload.samples.first_cycle);
}

// [test->fw~conn_server_006~1] a pass that outgrows the stage takes a second
// write rather than leaving records behind
static void test_a_backlog_pass_takes_a_second_write_not_less(void)
{
    useDeepTraceConfig();
    installFourFastWatches();
    resetTx();

    for (uint32_t c = 0U; c < 60U; c++)   // 3 ms of 16-byte records: ~1.2 kB on the wire
    {
        app_server_sampleCycle();
    }
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(2U, HW_USB_sim_txWrites());
    shared_Envelope replies[8];
    const uint32_t n = collectReplies(replies, 8U);
    uint32_t records = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        records += replies[i].payload.samples.count;
    }
    TEST_ASSERT_EQUAL_UINT32(60U, records);
}

// [test->fw~conn_server_001~1] the streams leave a reply's worth of transmit
// capacity, so a request during a saturating stream is answered in its pass.
// [test->fw~conn_trace_009~1]
static void test_reply_survives_a_saturating_sample_stream(void)
{
    useDeepTraceConfig();
    installFourFastWatches();

    // Eight passes with nothing drained on the host side fill the transport.
    for (uint32_t pass = 0U; pass < 8U; pass++)
    {
        for (uint32_t c = 0U; c < 20U; c++)
        {
            app_server_sampleCycle();
        }
        app_server_run1ms();
    }
    const uint32_t replyReserve = (uint32_t) IO_COBSFRAME_WIRE_MAX(LIB_PROTOBUF_ENVELOPE_MAX);
    TEST_ASSERT_TRUE(IO_serial_txFree(IO_SERIAL_CHANNEL_CDC) >= replyReserve);

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 77U;
    req.which_payload = shared_Envelope_trace_status_request_tag;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[64];
    const uint32_t n = collectReplies(replies, 64U);
    bool answered = false;
    for (uint32_t i = 0U; i < n; i++)
    {
        if ((replies[i].request_id == 77U) && (replies[i].which_payload == shared_Envelope_trace_status_tag))
        {
            answered = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(answered, "TraceStatus reply missing under a saturating stream");
}

// [test->fw~conn_server_001~1] a reply the transport cannot take is held and
// sent first once it can, ahead of any later request's reply.
static void test_reply_held_until_the_transport_takes_it(void)
{
    // Pack the transport with frame delimiters (parsed as empty segments).
    packTxTo(0U);

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 5U;
    req.which_payload = shared_Envelope_ping_tag;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 8U));   // held: no room yet

    req.request_id = 6U;
    injectEnvelope(&req);
    app_server_run1ms();                                          // room now: held reply first
    const uint32_t n = collectReplies(replies, 8U);
    TEST_ASSERT_TRUE(n >= 2U);
    TEST_ASSERT_EQUAL_UINT32(5U, replies[0].request_id);
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL_UINT32(6U, replies[1].request_id);
}

// [test->fw~conn_server_001~1] a second request arriving behind a held reply
// waits its turn: both are answered, in order, once the transport takes them.
static void test_two_pipelined_requests_are_both_answered(void)
{
    HW_USB_sim_setTxAccepting(false);

    shared_Envelope req = shared_Envelope_init_zero;
    req.which_payload = shared_Envelope_ping_tag;
    req.request_id = 11U;
    injectEnvelope(&req);
    req.request_id = 12U;
    injectEnvelope(&req);

    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(0U, HW_USB_sim_txLen());

    HW_USB_sim_setTxAccepting(true);
    app_server_run1ms();

    shared_Envelope replies[8];
    const uint32_t n = collectReplies(replies, 8U);
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_UINT32(11U, replies[0].request_id);
    TEST_ASSERT_TRUE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL_UINT32(12U, replies[1].request_id);
    TEST_ASSERT_TRUE(replies[1].payload.response.accepted);
}

// [test->fw~conn_server_001~1] a held reply belongs to its session: a
// disconnect drops it rather than answering the next host with it.
static void test_held_reply_dies_with_the_session(void)
{
    HW_USB_sim_setTxAccepting(false);

    shared_Envelope req = shared_Envelope_init_zero;
    req.request_id = 5U;
    req.which_payload = shared_Envelope_ping_tag;
    injectEnvelope(&req);
    app_server_run1ms();

    shared_Envelope replies[8];
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 8U));   // held: no room yet

    HW_USB_sim_setConnected(false);
    app_server_run1ms();                                          // disconnect edge
    HW_USB_sim_setTxAccepting(true);
    HW_USB_sim_setConnected(true);
    app_server_run1ms();
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(0U, collectReplies(replies, 8U));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_bad_config);

    RUN_TEST(test_ping_accepted_with_request_id);
    RUN_TEST(test_unrecognized_payload_rejected_with_cause);
    RUN_TEST(test_undecodable_frame_rejected_with_cause);
    RUN_TEST(test_board_request_forwarded_to_hook);
    RUN_TEST(test_board_request_without_hook_rejected);

    RUN_TEST(test_identity_request_returns_build_identity);

    RUN_TEST(test_telemetry_published_every_100th_tick);
    RUN_TEST(test_telemetry_skipped_when_hook_declines);

    RUN_TEST(test_log_strings_emitted_in_order);
    RUN_TEST(test_log_overflow_retains_newest);
    RUN_TEST(test_log_buffered_while_disconnected_emitted_on_connect);

    RUN_TEST(test_trace_init_rejects_bad_resources);

    RUN_TEST(test_watch_admission_accepted_reports_usage);
    RUN_TEST(test_watch_admission_rejects_bad_entries);
    RUN_TEST(test_watch_admission_five_one_cycle_entries_accepted);
    RUN_TEST(test_watch_admission_link_budget_boundary);
    RUN_TEST(test_watch_admission_ram_budget_rejection);
    RUN_TEST(test_watch_admission_ram_budget_boundary_fits);
    RUN_TEST(test_watch_admission_samples_capacity_rejection);
    RUN_TEST(test_rejected_request_leaves_prior_list_streaming);

    RUN_TEST(test_watch_list_clears_on_disconnect);
    RUN_TEST(test_stale_rx_dropped_on_disconnect);

    RUN_TEST(test_group_offsets_and_periods);
    RUN_TEST(test_capture_order_and_batching);
    RUN_TEST(test_slow_group_records_ride_their_own_message);
    RUN_TEST(test_sampler_quiet_without_a_list);
    RUN_TEST(test_new_list_restarts_stream_discarding_buffered);
    RUN_TEST(test_ring_overflow_skips_whole_records_leaving_gap);
    RUN_TEST(test_one_cycle_group_captures_a_coherent_snapshot);
    RUN_TEST(test_interior_gap_splits_the_batch);
    RUN_TEST(test_overflow_keeps_the_slow_group_records_honest);
    RUN_TEST(test_sixteen_byte_records_split_a_millisecond_as_sixteen_and_four);
    RUN_TEST(test_overflow_holds_until_the_buffer_drains_to_half);
    RUN_TEST(test_short_transmit_capacity_holds_records_whole);
    RUN_TEST(test_records_stalled_three_milliseconds_arrive_in_capture_order);
    RUN_TEST(test_no_record_is_held_past_two_emissions);

    RUN_TEST(test_samples_frame_is_byte_exact);
    RUN_TEST(test_max_samples_frame_layout_and_wire_bound);
    RUN_TEST(test_full_message_with_a_multi_byte_first_cycle_fits_the_bound);

    RUN_TEST(test_read_returns_current_contents);
    RUN_TEST(test_read_rejections);
    RUN_TEST(test_write_lands_and_reads_back);
    RUN_TEST(test_write_without_sampler_mask_hook_lands);
    RUN_TEST(test_write_rejections);

    RUN_TEST(test_link_test_streams_exactly_the_requested_frames);
    RUN_TEST(test_link_test_rejects_out_of_range_requests);
    RUN_TEST(test_link_test_rejects_second_request_while_running);
    RUN_TEST(test_link_test_count_reached_frees_the_service);
    RUN_TEST(test_link_test_clears_on_disconnect);
    RUN_TEST(test_envelope_assembled_around_a_payload_matches_the_whole);
    RUN_TEST(test_a_pass_leaves_as_one_write);
    RUN_TEST(test_a_backlog_pass_takes_a_second_write_not_less);
    RUN_TEST(test_reply_survives_a_saturating_sample_stream);
    RUN_TEST(test_reply_held_until_the_transport_takes_it);
    RUN_TEST(test_two_pipelined_requests_are_both_answered);
    RUN_TEST(test_held_reply_dies_with_the_session);

    return UNITY_END();
}
