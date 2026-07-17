#include "HW_SPI.h"
#include "HW_SPI_sim.h"
#include "HW_SPI_timeout.h"
#include "HW_GPIO.h"
#include "HW_GPIO_sim.h"
#include "SIL_ports.h"
#include "unity.h"

// Single-bit HAL-style pin masks for the two GPIO chip-selects used here.
#define CS1_PIN    (0x04U)   // AS5048_1 -> port C
#define CS2_PIN    (0x02U)   // AS5048_2 -> port B

// Test-owned SIL_ports hooks double: a linked duplex peer that answers every
// transfer with a canned frame, exercising the production upcall path. Installed
// before HW_SPI_init so each channel registers its duplex endpoint against it.
// With no hooks installed the bus is unlinked and a transfer reads all-ones.
static uint8_t canned[8];
static size_t  cannedLen;

static int32_t hookRegister(void * ctx, const char * sigType, const char * localName,
                            const char * modifier, const char * unit, int32_t kind)
{
    (void)ctx; (void)sigType; (void)localName; (void)modifier; (void)unit; (void)kind;
    return 0; // one valid handle for every endpoint; the tests drive one channel
}

static bool hookDuplex(void * ctx, int32_t handle, const uint8_t * tx, size_t txLen,
                       uint8_t * rx, size_t rxMax, size_t * rxLen)
{
    (void)ctx; (void)handle; (void)tx; (void)txLen;
    const size_t n = (cannedLen < rxMax) ? cannedLen : rxMax;
    for (size_t i = 0U; i < n; i++)
    {
        rx[i] = canned[i];
    }
    *rxLen = n;
    return true;
}

static void installDuplexPeer(const uint8_t * frame, size_t len)
{
    cannedLen = (len < sizeof(canned)) ? len : sizeof(canned);
    for (size_t i = 0U; i < cannedLen; i++)
    {
        canned[i] = frame[i];
    }
    const SIL_ports_hooks_S hooks = {
        .context        = NULL,
        .registerSignal = hookRegister,
        .readSignal     = NULL,
        .writeSignal    = NULL,
        .duplexTransfer = hookDuplex,
    };
    SIL_ports_setHooks(&hooks);
}

// File-scope config the tests build (good baseline) and tweak per case.
// HW_SPI_init stores a pointer to it, so it must outlive each test — hence
// file scope rather than a stack local.
static HW_SPI_busConfig_S     spiBuses[HW_SPI_BUS_COUNT];
static HW_SPI_channelConfig_S spiChannels[HW_SPI_CHANNEL_COUNT];
static HW_SPI_config_S        spiConfig;

// Completion-callback observation.
static uint32_t          cbCount;
static HW_SPI_channel_E  cbChannel;
static void *            cbContext;

static void testCallback(HW_SPI_channel_E channel, void * context)
{
    cbCount++;
    cbChannel = channel;
    cbContext = context;
}

// Good baseline: BUS_1 software, BUS_2 disabled, BUS_3 DMA (async).
// AS5048_1/2 share BUS_1 with distinct GPIO chip-selects of opposite
// polarity; SK6805 is the no-CS device on the async bus.
static void buildGoodConfig(void)
{
    spiBuses[HW_SPI_BUS_1] = (HW_SPI_busConfig_S){
        .enabled = true, .transferMode = HW_SPI_TRANSFERMODE_SW, .busNameStr = "B1" };
    spiBuses[HW_SPI_BUS_2] = (HW_SPI_busConfig_S){ .enabled = false };
    spiBuses[HW_SPI_BUS_3] = (HW_SPI_busConfig_S){
        .enabled = true, .transferMode = HW_SPI_TRANSFERMODE_DMA, .busNameStr = "B3" };

    spiChannels[HW_SPI_CHANNEL_AS5048_1] = (HW_SPI_channelConfig_S){
        .bus = HW_SPI_BUS_1, .csMode = HW_SPI_CS_MODE_GPIO,
        .csGpioConfig = { .port = HW_GPIO_PORT_C, .pin = CS1_PIN, .activeLevel = HW_GPIO_LEVEL_LOW },
        .channelNameStr = "as1" };
    spiChannels[HW_SPI_CHANNEL_AS5048_2] = (HW_SPI_channelConfig_S){
        .bus = HW_SPI_BUS_1, .csMode = HW_SPI_CS_MODE_GPIO,
        .csGpioConfig = { .port = HW_GPIO_PORT_B, .pin = CS2_PIN, .activeLevel = HW_GPIO_LEVEL_HIGH },
        .channelNameStr = "as2" };
    spiChannels[HW_SPI_CHANNEL_SK6805_STRING] = (HW_SPI_channelConfig_S){
        .bus = HW_SPI_BUS_3, .csMode = HW_SPI_CS_MODE_NONE, .channelNameStr = "sk" };

    spiConfig = (HW_SPI_config_S){
        .buses = spiBuses, .numBuses = HW_SPI_BUS_COUNT,
        .channels = spiChannels, .numChannels = HW_SPI_CHANNEL_COUNT };
}

void setUp(void)
{
    SIL_ports_setHooks(NULL); // unlinked bus by default; per-test peer opts in
    HW_GPIO_sim_reset();
    cbCount   = 0U;
    cbChannel = HW_SPI_CHANNEL_COUNT;
    cbContext = NULL;
    buildGoodConfig();
}

void tearDown(void)
{
    SIL_ports_setHooks(NULL);
}

/* ---- fw~hal_spi_001: init + config validation ---- */
// [test->fw~hal_spi_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_SPI_init(NULL));
}

// [test->fw~hal_spi_001~1]
static void test_init_channel_on_disabled_bus(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].bus = HW_SPI_BUS_2; // disabled
    TEST_ASSERT_FALSE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_001~1]
static void test_init_channel_on_out_of_range_bus(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].bus = HW_SPI_BUS_COUNT;
    TEST_ASSERT_FALSE(HW_SPI_init(&spiConfig));
}

/* ---- fw~hal_spi_007: chip-select configuration validity ---- */
// [test->fw~hal_spi_007~1]
static void test_cs_validity_gpio_out_of_range_port(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.port = HW_GPIO_PORT_COUNT;
    TEST_ASSERT_FALSE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_007~1]
static void test_cs_validity_gpio_multibit_pin(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.pin = 0x03U; // two bits set
    TEST_ASSERT_FALSE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_007~1]
static void test_cs_validity_gpio_zero_pin(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.pin = 0x00U;
    TEST_ASSERT_FALSE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_007~1]
static void test_cs_validity_gpio_single_valid_pin(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.pin  = 0x8000U; // highest valid line
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.port = HW_GPIO_PORT_A;
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
}

// [test->fw~hal_spi_007~1]
static void test_cs_validity_hw_mode_ignores_gpio_fields(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csMode = HW_SPI_CS_MODE_HW;
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.port = HW_GPIO_PORT_COUNT; // garbage
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csGpioConfig.pin  = 0x00U;              // garbage
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
}

/* ---- fw~hal_spi_002: logical channel addressing over shared buses ---- */
// [test->fw~hal_spi_002~1]
static void test_addressed_channel_asserts_only_its_own_cs(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0xAAU, 0x55U };
    uint8_t rx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmitReceive(HW_SPI_CHANNEL_AS5048_1, tx, rx, 2U));

    // Only AS5048_1's chip-select moved; AS5048_2 (same bus) untouched.
    TEST_ASSERT_EQUAL_UINT32(1U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_AS5048_1));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_AS5048_2));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_B, CS2_PIN));
    // Assert then deassert on the addressed line.
    TEST_ASSERT_EQUAL_UINT32(2U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_C, CS1_PIN));
    // Unlinked bus: MISO reads all-ones on the addressed channel.
    const uint8_t ones[2] = { 0xFFU, 0xFFU };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ones, rx, 2U);
}

// [test->fw~hal_spi_002~1]
static void test_second_channel_on_shared_bus_independent(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[3] = { 1U, 2U, 3U };
    uint8_t rx[3] = { 0U, 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmitReceive(HW_SPI_CHANNEL_AS5048_2, tx, rx, 3U));

    TEST_ASSERT_EQUAL_UINT32(1U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_AS5048_2));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_AS5048_1));
    // Unlinked bus: MISO reads all-ones.
    const uint8_t ones[3] = { 0xFFU, 0xFFU, 0xFFU };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ones, rx, 3U);
}

/* ---- fw~hal_spi_003: blocking byte transfers + computed timeout ---- */
// [test->fw~hal_spi_003~1]
static void test_transmit_moves_exact_length(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[4] = { 0x10U, 0x20U, 0x30U, 0x40U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 4U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_AS5048_1));

    uint8_t captured[4] = { 0U };
    const size_t n = HW_SPI_sim_getLastTx(HW_SPI_CHANNEL_AS5048_1, captured, 4U);
    TEST_ASSERT_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, captured, 4U);
}

// [test->fw~hal_spi_003~1]
static void test_receive_returns_linked_peer_frame(void)
{
    uint8_t frame[3] = { 0xDEU, 0xADU, 0xBEU };
    installDuplexPeer(frame, 3U); // link the peer before init so the channel registers its endpoint
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t rx[3] = { 0U, 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_receive(HW_SPI_CHANNEL_AS5048_1, rx, 3U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, rx, 3U);
}

// [test->fw~hal_spi_003~1]
static void test_transmitReceive_unlinked_bus_reads_ones(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig)); // no peer linked

    uint8_t tx[5] = { 9U, 8U, 7U, 6U, 5U };
    uint8_t rx[5] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmitReceive(HW_SPI_CHANNEL_AS5048_1, tx, rx, 5U));
    const uint8_t ones[5] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ones, rx, 5U);
}

// [test->fw~hal_spi_003~1]
static void test_software_transfer_timeout_returns_false(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    HW_SPI_sim_setStall(HW_SPI_CHANNEL_AS5048_1, true);
    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_FALSE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_ERROR, HW_SPI_getStatus(HW_SPI_CHANNEL_AS5048_1));
}

// [test->fw~hal_spi_003~1]
static void test_timeout_formula(void)
{
    // ceil(8800*N / f_bit) + 1ms.
    TEST_ASSERT_EQUAL_UINT32(2U,  HW_SPI_computeTimeoutMs(8000000U, 2U));    // 0.0022 -> 1, +1
    TEST_ASSERT_EQUAL_UINT32(2U,  HW_SPI_computeTimeoutMs(1000000U, 1U));    // 0.0088 -> 1, +1
    TEST_ASSERT_EQUAL_UINT32(10U, HW_SPI_computeTimeoutMs(1000000U, 1000U)); // 8.8  -> 9, +1
    TEST_ASSERT_EQUAL_UINT32(0U,  HW_SPI_computeTimeoutMs(0U, 100U));        // no bit rate
}

/* ---- fw~hal_spi_004: driver-managed chip-select with polarity ---- */
// [test->fw~hal_spi_004~1]
static void test_cs_active_low_polarity(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 1U));
    // Active-low: driven low to select, high to release.
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW,  HW_SPI_sim_getCsAssertLevel(HW_SPI_CHANNEL_AS5048_1));
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_HIGH, HW_SPI_sim_getCsDeassertLevel(HW_SPI_CHANNEL_AS5048_1));
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_HIGH, HW_GPIO_sim_getLevel(HW_GPIO_PORT_C, CS1_PIN));
}

// [test->fw~hal_spi_004~1]
static void test_cs_active_high_polarity(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_2, tx, 1U));
    // Active-high: the inverse of active-low.
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_HIGH, HW_SPI_sim_getCsAssertLevel(HW_SPI_CHANNEL_AS5048_2));
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW,  HW_SPI_sim_getCsDeassertLevel(HW_SPI_CHANNEL_AS5048_2));
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW,  HW_GPIO_sim_getLevel(HW_GPIO_PORT_B, CS2_PIN));
}

// [test->fw~hal_spi_004~1]
static void test_cs_hw_mode_drives_no_gpio(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csMode = HW_SPI_CS_MODE_HW;
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_AS5048_1));
    TEST_ASSERT_EQUAL_UINT32(0U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_C, CS1_PIN));
}

// [test->fw~hal_spi_004~1]
static void test_cs_none_mode_drives_no_gpio(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_UINT32(0U, HW_SPI_sim_getCsAssertCount(HW_SPI_CHANNEL_SK6805_STRING));
}

/* ---- fw~hal_spi_005: asynchronous transfer completion model ---- */
// [test->fw~hal_spi_005~1]
static void test_async_busy_then_complete_with_callback(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    int ctx = 0;
    TEST_ASSERT_TRUE(HW_SPI_registerCallback(HW_SPI_CHANNEL_SK6805_STRING, testCallback, &ctx));

    uint8_t tx[2] = { 0x11U, 0x22U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));

    // Returns immediately; completion is pending.
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount);

    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_SPI_CHANNEL_SK6805_STRING, cbChannel);
    TEST_ASSERT_EQUAL_PTR(&ctx, cbContext);

    // Callback fires exactly once.
    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
}

// [test->fw~hal_spi_005~1]
static void test_async_error_completion(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    int ctx = 0;
    TEST_ASSERT_TRUE(HW_SPI_registerCallback(HW_SPI_CHANNEL_SK6805_STRING, testCallback, &ctx));
    HW_SPI_sim_setForceError(HW_SPI_CHANNEL_SK6805_STRING, true);

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    HW_SPI_sim_tick();

    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_ERROR, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
}

// [test->fw~hal_spi_005~1]
static void test_async_observable_by_polling_only(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));

    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount); // no callback registered
}

/* ---- fw~hal_spi_006: transfer-mode taxonomy ---- */
// [test->fw~hal_spi_006~1]
static void test_mode_software_completes(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_AS5048_1));
}

// [test->fw~hal_spi_006~1]
static void test_mode_dma_completes(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
}

// [test->fw~hal_spi_006~1]
static void test_mode_interrupt_completes(void)
{
    spiBuses[HW_SPI_BUS_3].transferMode = HW_SPI_TRANSFERMODE_INTERRUPT;
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    HW_SPI_sim_tick();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_channel_on_disabled_bus);
    RUN_TEST(test_init_channel_on_out_of_range_bus);

    RUN_TEST(test_cs_validity_gpio_out_of_range_port);
    RUN_TEST(test_cs_validity_gpio_multibit_pin);
    RUN_TEST(test_cs_validity_gpio_zero_pin);
    RUN_TEST(test_cs_validity_gpio_single_valid_pin);
    RUN_TEST(test_cs_validity_hw_mode_ignores_gpio_fields);

    RUN_TEST(test_addressed_channel_asserts_only_its_own_cs);
    RUN_TEST(test_second_channel_on_shared_bus_independent);

    RUN_TEST(test_transmit_moves_exact_length);
    RUN_TEST(test_receive_returns_linked_peer_frame);
    RUN_TEST(test_transmitReceive_unlinked_bus_reads_ones);
    RUN_TEST(test_software_transfer_timeout_returns_false);
    RUN_TEST(test_timeout_formula);

    RUN_TEST(test_cs_active_low_polarity);
    RUN_TEST(test_cs_active_high_polarity);
    RUN_TEST(test_cs_hw_mode_drives_no_gpio);
    RUN_TEST(test_cs_none_mode_drives_no_gpio);

    RUN_TEST(test_async_busy_then_complete_with_callback);
    RUN_TEST(test_async_error_completion);
    RUN_TEST(test_async_observable_by_polling_only);

    RUN_TEST(test_mode_software_completes);
    RUN_TEST(test_mode_dma_completes);
    RUN_TEST(test_mode_interrupt_completes);

    return UNITY_END();
}
