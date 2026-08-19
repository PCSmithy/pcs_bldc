#include "app_server.h"
#include "IO_serial.h"
#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "lib_cobs.h"
#include "lib_crc32.h"
#include "lib_protobuf.h"
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
#define TRACE_LINK_BUDGET     (1100000U)

static uint32_t traceMemory[64];
static app_server_watch_S traceWatchStorage[2U * TRACE_WATCH_CAPACITY];
static uint8_t traceSampleStorage[TRACE_RAM_BUDGET];
static const app_server_region_S traceReadableRegions[] = {
    { .start = TRACE_TEST_BASE, .length = sizeof(traceMemory), .base = (uintptr_t) traceMemory },
};
static const app_server_region_S traceWritableRegions[] = {
    { .start = TRACE_TEST_BASE, .length = sizeof(traceMemory) / 2U, .base = (uintptr_t) traceMemory },
};

// Oversized fixtures for the list-level admission bounds (Samples capacity)
// and the max-size Samples frame, unreachable at the default capacity of 8.
#define TRACE_BIG_WATCH_CAPACITY (80U)
static app_server_watch_S traceBigWatchStorage[2U * TRACE_BIG_WATCH_CAPACITY];
static uint8_t traceBigSampleStorage[1024U];

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
    serverConfig.sampleRamBudgetBytes = sizeof(traceBigSampleStorage);
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
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
        { .address = TRACE_TEST_BASE,       .size = 4U, .period_ms = 1U  },
        { .address = TRACE_TEST_BASE + 16U, .size = 2U, .period_ms = 10U },
    };
    const trace_TraceStatus status = installWatches(watches, 2U);
    TEST_ASSERT_EQUAL_UINT32(TRACE_RAM_BUDGET, status.ram_budget_bytes);
    TEST_ASSERT_EQUAL_UINT32(4U + 6U, status.ram_worst_tick_bytes);
    TEST_ASSERT_EQUAL_UINT32(TRACE_LINK_BUDGET, status.link_budget_bytes_per_s);
    // r = 4 B x 1000 Hz + 2 B x 100 Hz + 21 B overhead x 1000 Hz
    TEST_ASSERT_EQUAL_UINT32(25200U, status.link_rate_bytes_per_s);
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_rejects_bad_entries(void)
{
    const trace_Watch outside = {
        .address = TRACE_TEST_BASE + sizeof(traceMemory) - 2U, .size = 4U, .period_ms = 1U };
    expectWatchRejection(&outside, 1U, "not readable");

    const trace_Watch sizeZero = { .address = TRACE_TEST_BASE, .size = 0U, .period_ms = 1U };
    expectWatchRejection(&sizeZero, 1U, "size");

    const trace_Watch sizeBig = { .address = TRACE_TEST_BASE, .size = 9U, .period_ms = 1U };
    expectWatchRejection(&sizeBig, 1U, "size");

    const trace_Watch badPeriod = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 7U };
    expectWatchRejection(&badPeriod, 1U, "period");

    trace_Watch tooMany[TRACE_WATCH_CAPACITY + 1U];
    for (uint32_t i = 0U; i < (TRACE_WATCH_CAPACITY + 1U); i++)
    {
        tooMany[i] = (trace_Watch){ .address = TRACE_TEST_BASE, .size = 1U, .period_ms = 100U };
    }
    expectWatchRejection(tooMany, TRACE_WATCH_CAPACITY + 1U, "list exceeds");
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_link_budget_boundary(void)
{
    // One 4-byte 1 ms watch: r = 4000 + 21 x 1000 = 25000 exactly.
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 1U };

    serverConfig.linkBudgetBytesPerS = 25000U;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    (void) installWatches(&w, 1U);   // asserts acceptance at r == budget

    serverConfig.linkBudgetBytesPerS = 24999U;
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    expectWatchRejection(&w, 1U, "link");
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_ram_budget_rejection(void)
{
    serverConfig.sampleRamBudgetBytes = 9U;   // u = 4 + 8 = 12 > 9
    TEST_ASSERT_TRUE(app_server_init(&serverConfig));
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 8U, .period_ms = 100U };
    expectWatchRejection(&w, 1U, "RAM");
}

// [test->fw~conn_trace_002~1]
static void test_watch_admission_samples_capacity_rejection(void)
{
    useBigTraceConfig();
    trace_Watch watches[33];
    for (uint32_t i = 0U; i < 33U; i++)
    {
        watches[i] = (trace_Watch){ .address = TRACE_TEST_BASE, .size = 8U, .period_ms = 100U };
    }
    // 33 x 8 = 264 data bytes > the 256-byte Samples capacity
    expectWatchRejection(watches, 33U, "Samples");
}

// [test->fw~conn_trace_002~1]
static void test_rejected_request_leaves_prior_list_streaming(void)
{
    traceMemory[0] = 0xAABBCCDDU;
    const trace_Watch good = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 1U };
    (void) installWatches(&good, 1U);

    app_server_sample1ms();   // tick 0 buffered

    const trace_Watch bad = { .address = TRACE_TEST_BASE, .size = 0U, .period_ms = 1U };
    injectWatchRequest(31U, &bad, 1U);
    app_server_run1ms();      // pump rejects, then the drain emits tick 0

    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(2U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_response_tag, replies[0].which_payload);
    TEST_ASSERT_FALSE(replies[0].payload.response.accepted);
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[1].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[1].payload.samples.tick_ms);

    app_server_sample1ms();   // the prior list is still live
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(1U, replies[0].payload.samples.tick_ms);
}

/* ---- fw~conn_trace_003: watch-list clear on disconnect ---- */

// [test->fw~conn_trace_003~1]
static void test_watch_list_clears_on_disconnect(void)
{
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 1U };
    (void) installWatches(&w, 1U);
    app_server_sample1ms();

    HW_USB_sim_setConnected(false);
    app_server_run1ms();   // disconnect edge: list + buffered samples die
    HW_USB_sim_setConnected(true);

    app_server_sample1ms();
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
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.trace_status.ram_worst_tick_bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.trace_status.link_rate_bytes_per_s);
}

/* ---- fw~conn_trace_004: sampling ---- */

// [test->fw~conn_trace_004~1]
static void test_sampling_due_rule_and_order(void)
{
    traceMemory[0] = 0x11223344U;
    traceMemory[4] = 0x0000BEEFU;
    const trace_Watch watches[] = {
        { .address = TRACE_TEST_BASE,       .size = 4U, .period_ms = 1U  },
        { .address = TRACE_TEST_BASE + 16U, .size = 2U, .period_ms = 10U },
    };
    (void) installWatches(watches, 2U);

    for (uint32_t i = 0U; i < 20U; i++)
    {
        app_server_sample1ms();
    }
    app_server_run1ms();

    shared_Envelope replies[24];
    TEST_ASSERT_EQUAL_UINT32(20U, collectReplies(replies, 24U));
    for (uint32_t t = 0U; t < 20U; t++)
    {
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[t].which_payload);
        TEST_ASSERT_EQUAL_UINT32(t, replies[t].payload.samples.tick_ms);
        const bool slowDue = ((t % 10U) == 0U);
        TEST_ASSERT_EQUAL_UINT32(((slowDue)) ? 6U : 4U, replies[t].payload.samples.data.size);
        TEST_ASSERT_EQUAL_UINT8(0x44U, replies[t].payload.samples.data.bytes[0]);
        TEST_ASSERT_EQUAL_UINT8(0x11U, replies[t].payload.samples.data.bytes[3]);
        if (slowDue)
        {
            TEST_ASSERT_EQUAL_UINT8(0xEFU, replies[t].payload.samples.data.bytes[4]);
            TEST_ASSERT_EQUAL_UINT8(0xBEU, replies[t].payload.samples.data.bytes[5]);
        }
    }
}

// [test->fw~conn_trace_004~1]
static void test_new_list_restarts_stream_discarding_buffered(void)
{
    const trace_Watch first = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 1U };
    (void) installWatches(&first, 1U);
    for (uint32_t i = 0U; i < 3U; i++)
    {
        app_server_sample1ms();   // ticks 0..2 buffered, never drained
    }

    const trace_Watch second = { .address = TRACE_TEST_BASE + 4U, .size = 2U, .period_ms = 1U };
    // installWatches asserts exactly one reply: the buffered prior-list ticks
    // were discarded by the install, not drained after it.
    (void) installWatches(&second, 1U);

    app_server_sample1ms();
    app_server_run1ms();
    shared_Envelope replies[4];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 4U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.samples.tick_ms);
    TEST_ASSERT_EQUAL_UINT32(2U, replies[0].payload.samples.data.size);
}

// [test->fw~conn_trace_004~1]
static void test_ring_overflow_skips_whole_ticks_leaving_gap(void)
{
    const trace_Watch w = { .address = TRACE_TEST_BASE, .size = 4U, .period_ms = 1U };
    (void) installWatches(&w, 1U);

    // Record = 10 B, ring free = 255 B: ticks 0..24 fit, 25..29 are skipped.
    for (uint32_t i = 0U; i < 30U; i++)
    {
        app_server_sample1ms();
    }
    app_server_run1ms();
    shared_Envelope replies[32];
    TEST_ASSERT_EQUAL_UINT32(25U, collectReplies(replies, 32U));
    for (uint32_t t = 0U; t < 25U; t++)
    {
        TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[t].which_payload);
        TEST_ASSERT_EQUAL_UINT32(t, replies[t].payload.samples.tick_ms);
        TEST_ASSERT_EQUAL_UINT32(4U, replies[t].payload.samples.data.size);
    }

    app_server_sample1ms();   // the tick count jumps the gap
    app_server_run1ms();
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 32U));
    TEST_ASSERT_EQUAL_UINT32(30U, replies[0].payload.samples.tick_ms);
}

/* ---- fw~conn_trace_005: Samples message format ---- */

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
            .address = TRACE_TEST_BASE + (i * 8U), .size = 8U, .period_ms = 100U };
    }
    (void) installWatches(watches, 32U);

    app_server_sample1ms();   // tick 0: all 32 due, 256 data bytes
    app_server_run1ms();

    // Whole wire frame within data + W: 256 + 21.
    uint8_t wire[2048];
    const uint32_t wireLen = HW_USB_sim_readTx(wire, sizeof(wire));
    TEST_ASSERT_TRUE(wireLen > 256U);
    TEST_ASSERT_TRUE(wireLen <= 277U);

    // And the payload is the watched spans concatenated in list order.
    shared_Envelope replies[2];
    TEST_ASSERT_EQUAL_UINT32(1U, collectReplies(replies, 2U));
    TEST_ASSERT_EQUAL(shared_Envelope_samples_tag, replies[0].which_payload);
    TEST_ASSERT_EQUAL_UINT32(0U, replies[0].payload.samples.tick_ms);
    TEST_ASSERT_EQUAL_UINT32(256U, replies[0].payload.samples.data.size);
    for (uint32_t i = 0U; i < 32U; i++)
    {
        TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *) &traceMemory[i * 2U],
                                      &replies[0].payload.samples.data.bytes[i * 8U], 8U);
    }
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

    RUN_TEST(test_trace_init_rejects_bad_resources);

    RUN_TEST(test_watch_admission_accepted_reports_usage);
    RUN_TEST(test_watch_admission_rejects_bad_entries);
    RUN_TEST(test_watch_admission_link_budget_boundary);
    RUN_TEST(test_watch_admission_ram_budget_rejection);
    RUN_TEST(test_watch_admission_samples_capacity_rejection);
    RUN_TEST(test_rejected_request_leaves_prior_list_streaming);

    RUN_TEST(test_watch_list_clears_on_disconnect);

    RUN_TEST(test_sampling_due_rule_and_order);
    RUN_TEST(test_new_list_restarts_stream_discarding_buffered);
    RUN_TEST(test_ring_overflow_skips_whole_ticks_leaving_gap);

    RUN_TEST(test_max_samples_frame_layout_and_wire_bound);

    RUN_TEST(test_read_returns_current_contents);
    RUN_TEST(test_read_rejections);
    RUN_TEST(test_write_lands_and_reads_back);
    RUN_TEST(test_write_rejections);

    return UNITY_END();
}
