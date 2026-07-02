#include "HW_DMA.h"
#include "HW_DMA_sim.h"
#include "unity.h"

// File-scope config the tests build (good baseline) and tweak per case.
// HW_DMA_init stores a pointer to it, so it must outlive each test.
static HW_DMA_channelConfig_S dmaChannels[HW_DMA_CHANNEL_COUNT];
static HW_DMA_config_S        dmaConfig;

// Completion-callback observation.
static uint32_t         cbCount;
static HW_DMA_channel_E cbChannel;
static void *           cbContext;

static void testCallback(HW_DMA_channel_E channel, void * context)
{
    cbCount++;
    cbChannel = channel;
    cbContext = context;
}

// SK6805_TX is memory-to-peripheral; AS5048_RX is peripheral-to-memory. Both
// 8-bit, so item count equals byte count in these tests.
static void buildGoodConfig(void)
{
    dmaChannels[HW_DMA_CHANNEL_SK6805_TX] = (HW_DMA_channelConfig_S){
        .direction = HW_DMA_DIRECTION_MEM_TO_PERIPH, .width = HW_DMA_WIDTH_8BIT, .channelNameStr = "tx" };
    dmaChannels[HW_DMA_CHANNEL_AS5048_RX] = (HW_DMA_channelConfig_S){
        .direction = HW_DMA_DIRECTION_PERIPH_TO_MEM, .width = HW_DMA_WIDTH_8BIT, .channelNameStr = "rx" };

    dmaConfig = (HW_DMA_config_S){ .channels = dmaChannels, .numChannels = HW_DMA_CHANNEL_COUNT };
}

void setUp(void)
{
    HW_DMA_sim_reset();
    cbCount   = 0U;
    cbChannel = HW_DMA_CHANNEL_COUNT;
    cbContext = NULL;
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- fw~hal_dma_001: init + config validation ---- */

// [test->fw~hal_dma_001~1]
static void test_init_null_config_false(void)
{
    TEST_ASSERT_FALSE(HW_DMA_init(NULL));
}

// [test->fw~hal_dma_001~1]
static void test_init_valid_config_true(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));
}

// [test->fw~hal_dma_001~1]
static void test_init_rejects_bad_direction(void)
{
    dmaChannels[HW_DMA_CHANNEL_SK6805_TX].direction = (HW_DMA_direction_E)99;
    TEST_ASSERT_FALSE(HW_DMA_init(&dmaConfig));
}

// [test->fw~hal_dma_001~1]
static void test_init_rejects_bad_width(void)
{
    dmaChannels[HW_DMA_CHANNEL_AS5048_RX].width = (HW_DMA_width_E)99;
    TEST_ASSERT_FALSE(HW_DMA_init(&dmaConfig));
}

/* ---- fw~hal_dma_002: single-shot memory<->peripheral transfer ---- */

// [test->fw~hal_dma_002~1]
static void test_mem_to_periph_delivers_in_order(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    uint8_t buf[4] = { 1U, 2U, 3U, 4U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 4U));

    uint8_t captured[4] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(4U, HW_DMA_sim_getLastMemoryData(HW_DMA_CHANNEL_SK6805_TX, captured, 4U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, captured, 4U);
}

// [test->fw~hal_dma_002~1]
static void test_periph_to_mem_fills_in_order(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    const uint8_t injected[3] = { 10U, 20U, 30U };
    HW_DMA_sim_setInjectedPeriphData(HW_DMA_CHANNEL_AS5048_RX, injected, 3U);

    uint8_t buf[3] = { 0U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_AS5048_RX, buf, 3U));
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_UINT8_ARRAY(injected, buf, 3U);
}

// [test->fw~hal_dma_002~1]
static void test_start_rejects_bad_args(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    uint8_t buf[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_COUNT, buf, 2U));   // out-of-range channel
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, NULL, 2U)); // null buffer
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 0U));  // zero count
}

/* ---- fw~hal_dma_003: asynchronous transfer completion ---- */

// [test->fw~hal_dma_003~1]
static void test_started_transfer_reports_busy(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_BUSY, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

// [test->fw~hal_dma_003~1]
static void test_successful_transfer_completes(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_COMPLETE, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

// [test->fw~hal_dma_003~1]
static void test_failed_transfer_reports_error(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    HW_DMA_sim_setForceError(HW_DMA_CHANNEL_SK6805_TX, true);
    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_ERROR, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

// [test->fw~hal_dma_003~1]
static void test_callback_invoked_exactly_once(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    int ctx = 0;
    TEST_ASSERT_TRUE(HW_DMA_registerCallback(HW_DMA_CHANNEL_SK6805_TX, testCallback, &ctx));

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_DMA_CHANNEL_SK6805_TX, cbChannel);
    TEST_ASSERT_EQUAL_PTR(&ctx, cbContext);

    // A second tick with no pending transfer fires nothing more.
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
}

// [test->fw~hal_dma_003~1]
static void test_completion_observable_by_polling_without_callback(void)
{
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));

    // No callback registered: completion is still observable by polling status.
    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    HW_DMA_sim_tick();
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_COMPLETE, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_null_config_false);
    RUN_TEST(test_init_valid_config_true);
    RUN_TEST(test_init_rejects_bad_direction);
    RUN_TEST(test_init_rejects_bad_width);

    RUN_TEST(test_mem_to_periph_delivers_in_order);
    RUN_TEST(test_periph_to_mem_fills_in_order);
    RUN_TEST(test_start_rejects_bad_args);

    RUN_TEST(test_started_transfer_reports_busy);
    RUN_TEST(test_successful_transfer_completes);
    RUN_TEST(test_failed_transfer_reports_error);
    RUN_TEST(test_callback_invoked_exactly_once);
    RUN_TEST(test_completion_observable_by_polling_without_callback);

    return UNITY_END();
}
