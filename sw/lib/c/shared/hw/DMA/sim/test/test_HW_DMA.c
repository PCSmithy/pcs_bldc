#include "HW_DMA.h"
#include "SIL_irq.h"
#include "unity.h"

// File-scope config the tests build (good baseline) and tweak per case.
// HW_DMA_init stores a pointer to it, so it must outlive each test.
static HW_DMA_channelConfig_S dmaChannels[HW_DMA_CHANNEL_COUNT];
static HW_DMA_config_S        dmaConfig;

// Fake SIL_irq hooks: record the pended registration (capturing the handler
// so tests can run the completion themselves), pends, and cancels.
static SIL_irq_handler_F pendedHandler;
static uint32_t          pendedRegisterCalls;
static int32_t           pendedRegisterReturn;
static int32_t           lastPendHandle;
static uint32_t          pendCalls;
static int32_t           lastCancelHandle;
static uint32_t          cancelCalls;

static int32_t fakeRegisterPended(void * context, SIL_irq_handler_F handler, uint8_t priority)
{
    (void)context; (void)priority;
    pendedHandler = handler;
    pendedRegisterCalls++;
    return pendedRegisterReturn;
}

static void fakePend(void * context, int32_t handle)
{
    (void)context;
    lastPendHandle = handle;
    pendCalls++;
}

static void fakeCancel(void * context, int32_t handle)
{
    (void)context;
    lastCancelHandle = handle;
    cancelCalls++;
}

static void installIrqDouble(void)
{
    const SIL_irq_hooks_S hooks = {
        .registerPended = fakeRegisterPended,
        .pend           = fakePend,
        .cancel         = fakeCancel,
    };
    SIL_irq_setHooks(&hooks);
}

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

// Init behind the irq double: the completion service registers through it,
// handing the test the handler it invokes as the completion interrupt.
static void initWithIrqDouble(void)
{
    installIrqDouble();
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));
}

void setUp(void)
{
    // A rejected init is the clean slate: init drops the driver to its
    // uninitialized state before it looks at the config.
    SIL_irq_setHooks(NULL);
    (void)HW_DMA_init(NULL);
    buildGoodConfig();

    pendedHandler        = NULL;
    pendedRegisterCalls  = 0U;
    pendedRegisterReturn = 7;
    lastPendHandle       = SIL_IRQ_HANDLE_INVALID;
    pendCalls            = 0U;
    lastCancelHandle     = SIL_IRQ_HANDLE_INVALID;
    cancelCalls          = 0U;
    cbCount              = 0U;
    cbChannel            = HW_DMA_CHANNEL_COUNT;
    cbContext            = NULL;
}

void tearDown(void)
{
    SIL_irq_setHooks(NULL);
}

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

// [test->fw~hal_dma_001~1]
static void test_reinit_rewires_completion_and_clears_state(void)
{
    initWithIrqDouble();
    TEST_ASSERT_EQUAL_UINT32(1U, pendedRegisterCalls);

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));

    // Re-init: the old completion IRQ is cancelled, a fresh one registered,
    // and the in-flight transfer is gone with the rest of the state.
    TEST_ASSERT_TRUE(HW_DMA_init(&dmaConfig));
    TEST_ASSERT_EQUAL_UINT32(1U, cancelCalls);
    TEST_ASSERT_EQUAL_INT32(pendedRegisterReturn, lastCancelHandle);
    TEST_ASSERT_EQUAL_UINT32(2U, pendedRegisterCalls);
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_IDLE, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

/* ---- fw~hal_dma_002: single-shot memory<->peripheral transfer ---- */

// [test->fw~hal_dma_002~1]
static void test_periph_to_mem_fills_in_order(void)
{
    initWithIrqDouble();

    // No injected data: the completion fills the buffer with the synthetic
    // byte ramp, overwriting the sentinel bytes in order.
    uint8_t buf[4] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_AS5048_RX, buf, 4U));
    pendedHandler();

    const uint8_t expected[4] = { 0U, 1U, 2U, 3U };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, 4U);
}

// [test->fw~hal_dma_002~1]
static void test_start_rejects_bad_args(void)
{
    uint8_t buf[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U)); // uninitialized

    initWithIrqDouble();

    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_COUNT, buf, 2U));      // out-of-range channel
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, NULL, 2U)); // null buffer
    TEST_ASSERT_FALSE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 0U));  // zero count
    TEST_ASSERT_EQUAL_UINT32(0U, pendCalls);                                     // nothing started
}

/* ---- fw~hal_dma_003: asynchronous transfer completion ---- */

// [test->fw~hal_dma_003~1]
static void test_started_transfer_reports_busy_and_pends_completion(void)
{
    initWithIrqDouble();

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_BUSY, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
    TEST_ASSERT_EQUAL_UINT32(1U, pendCalls);
    TEST_ASSERT_EQUAL_INT32(pendedRegisterReturn, lastPendHandle);
}

// [test->fw~hal_dma_003~1]
static void test_successful_transfer_completes(void)
{
    initWithIrqDouble();

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    pendedHandler();
    TEST_ASSERT_EQUAL_INT(HW_DMA_STATUS_COMPLETE, HW_DMA_getStatus(HW_DMA_CHANNEL_SK6805_TX));
}

// [test->fw~hal_dma_003~1]
static void test_callback_invoked_exactly_once(void)
{
    initWithIrqDouble();

    int ctx = 0;
    TEST_ASSERT_TRUE(HW_DMA_registerCallback(HW_DMA_CHANNEL_SK6805_TX, testCallback, &ctx));

    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_DMA_CHANNEL_SK6805_TX, cbChannel);
    TEST_ASSERT_EQUAL_PTR(&ctx, cbContext);

    // A spurious completion with no pending transfer fires nothing more.
    pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
}

// [test->fw~hal_dma_003~1]
static void test_completion_observable_by_polling_without_callback(void)
{
    initWithIrqDouble();

    // No callback registered: completion is still observable by polling status.
    uint8_t buf[2] = { 5U, 6U };
    TEST_ASSERT_TRUE(HW_DMA_startTransfer(HW_DMA_CHANNEL_SK6805_TX, buf, 2U));
    pendedHandler();
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
    RUN_TEST(test_reinit_rewires_completion_and_clears_state);

    RUN_TEST(test_periph_to_mem_fills_in_order);
    RUN_TEST(test_start_rejects_bad_args);

    RUN_TEST(test_started_transfer_reports_busy_and_pends_completion);
    RUN_TEST(test_successful_transfer_completes);
    RUN_TEST(test_callback_invoked_exactly_once);
    RUN_TEST(test_completion_observable_by_polling_without_callback);

    return UNITY_END();
}
