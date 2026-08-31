#include "HW_SPI.h"
#include "HW_SPI_timeout.h"
#include "HW_GPIO.h"
#include "SIL_irq.h"
#include "SIL_ports.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

// Single-bit HAL-style pin masks for the two GPIO chip-selects used here.
#define CS1_PIN    (0x04U)   // AS5048_1 -> port C
#define CS2_PIN    (0x02U)   // AS5048_2 -> port B

// Test-owned SIL_ports hooks double, installed for every test so both drivers
// bind to the production seam: registerSignal hands out sequential handles and
// remembers each port's local name, writeSignal records the level sequence the
// sim GPIO publishes (how CS activity is observed), and duplexTransfer answers
// a transfer with a canned frame once a peer is linked — unlinked, it declines
// and the driver falls back to the floating-bus all-ones fill.
#define MAX_PORTS   (8)
#define MAX_WRITES  (4)
static char     portName[MAX_PORTS][16];
static double   portWrite[MAX_PORTS][MAX_WRITES];
static uint32_t portWrites[MAX_PORTS];
static int32_t  portCount;

static uint8_t canned[8];
static size_t  cannedLen;

static int32_t hookRegister(void * ctx, const char * sigType, const char * localName,
                            const char * unit, int32_t kind)
{
    (void)ctx; (void)sigType; (void)unit; (void)kind;
    int32_t handle = portCount;
    if (portCount < MAX_PORTS)
    {
        snprintf(portName[handle], sizeof(portName[handle]), "%s", localName);
        portCount++;
    }
    return handle;
}

static void hookWrite(void * ctx, int32_t handle, double value)
{
    (void)ctx;
    if ((handle >= 0) && (handle < MAX_PORTS))
    {
        if (portWrites[handle] < MAX_WRITES)
        {
            portWrite[handle][portWrites[handle]] = value;
        }
        portWrites[handle]++;
    }
}

static bool hookDuplex(void * ctx, int32_t handle, const uint8_t * tx, size_t txLen,
                       uint8_t * rx, size_t rxMax, size_t * rxLen)
{
    (void)ctx; (void)handle; (void)tx; (void)txLen;
    bool ret = false;
    if (cannedLen > 0U)
    {
        const size_t n = (cannedLen < rxMax) ? cannedLen : rxMax;
        for (size_t i = 0U; i < n; i++)
        {
            rx[i] = canned[i];
        }
        *rxLen = n;
        ret = true;
    }
    return ret;
}

static void installHooks(void)
{
    const SIL_ports_hooks_S hooks = {
        .context        = NULL,
        .registerSignal = hookRegister,
        .readSignal     = NULL,
        .writeSignal    = hookWrite,
        .duplexTransfer = hookDuplex,
    };
    SIL_ports_setHooks(&hooks);
}

// Fake SIL_irq hooks: record the pended registration (capturing the completion
// handler so tests can run it themselves), pends, and cancels.
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

// Link a duplex peer that answers every transfer with `frame`.
static void installDuplexPeer(const uint8_t * frame, size_t len)
{
    cannedLen = (len < sizeof(canned)) ? len : sizeof(canned);
    for (size_t i = 0U; i < cannedLen; i++)
    {
        canned[i] = frame[i];
    }
}

// Resolve a registered port's handle by its local name; -1 when absent.
static int32_t portHandle(const char * name)
{
    int32_t handle = -1;
    for (int32_t i = 0; (i < portCount) && (handle < 0); i++)
    {
        if (strcmp(portName[i], name) == 0)
        {
            handle = i;
        }
    }
    return handle;
}

// Drop the GPIO boot-state publication so a test counts only transfer traffic.
static void clearPortWrites(void)
{
    for (int32_t i = 0; i < MAX_PORTS; i++)
    {
        portWrites[i] = 0U;
    }
}

// The two chip-select lines as configured output pins, so the sim GPIO driver
// publishes each transfer's CS activity on their observation ports.
static HW_GPIO_pinConfig_S csPinB[1];
static HW_GPIO_pinConfig_S csPinC[1];
static HW_GPIO_config_S    gpioConfig;

static void buildGpioConfig(void)
{
    csPinB[0] = (HW_GPIO_pinConfig_S){
        .pin = CS2_PIN, .mode = HW_GPIO_MODE_OUTPUT, .pinNameStr = "CS2" };
    csPinC[0] = (HW_GPIO_pinConfig_S){
        .pin = CS1_PIN, .mode = HW_GPIO_MODE_OUTPUT, .pinNameStr = "CS1" };

    gpioConfig = (HW_GPIO_config_S){ 0 };
    gpioConfig.ports[HW_GPIO_PORT_B].pins    = csPinB;
    gpioConfig.ports[HW_GPIO_PORT_B].numPins = 1U;
    gpioConfig.ports[HW_GPIO_PORT_C].pins    = csPinC;
    gpioConfig.ports[HW_GPIO_PORT_C].numPins = 1U;
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
    // A rejected init is the clean slate: init drops the driver to its
    // uninitialized state before it looks at the config.
    SIL_irq_setHooks(NULL);
    (void)HW_SPI_init(NULL);

    for (int32_t i = 0; i < MAX_PORTS; i++)
    {
        portName[i][0] = '\0';
        portWrites[i]  = 0U;
    }
    portCount = 0;
    cannedLen = 0U; // unlinked bus by default; per-test peer opts in
    installHooks();

    pendedHandler        = NULL;
    pendedRegisterCalls  = 0U;
    pendedRegisterReturn = 11;
    lastPendHandle       = SIL_IRQ_HANDLE_INVALID;
    pendCalls            = 0U;
    lastCancelHandle     = SIL_IRQ_HANDLE_INVALID;
    cancelCalls          = 0U;
    installIrqDouble();

    // Re-entrant GPIO init is the clean slate (no _sim reset).
    buildGpioConfig();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    cbCount   = 0U;
    cbChannel = HW_SPI_CHANNEL_COUNT;
    cbContext = NULL;
    buildGoodConfig();
}

void tearDown(void)
{
    SIL_ports_setHooks(NULL);
    SIL_irq_setHooks(NULL);
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
    const int32_t cs1 = portHandle("CS1");
    const int32_t cs2 = portHandle("CS2");
    TEST_ASSERT_TRUE((cs1 >= 0) && (cs2 >= 0));
    clearPortWrites();

    uint8_t tx[2] = { 0xAAU, 0x55U };
    uint8_t rx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmitReceive(HW_SPI_CHANNEL_AS5048_1, tx, rx, 2U));

    // Assert then deassert on the addressed line; AS5048_2 (same bus) untouched.
    TEST_ASSERT_EQUAL_UINT32(2U, portWrites[cs1]);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[cs2]);
    // Unlinked bus: MISO reads all-ones on the addressed channel.
    const uint8_t ones[2] = { 0xFFU, 0xFFU };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ones, rx, 2U);
}

/* ---- fw~hal_spi_003: blocking byte transfers + computed timeout ---- */
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
    const int32_t cs1 = portHandle("CS1");
    TEST_ASSERT_TRUE(cs1 >= 0);
    clearPortWrites();

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 1U));
    // Active-low: driven low to select, high to release.
    TEST_ASSERT_EQUAL_UINT32(2U, portWrites[cs1]);
    TEST_ASSERT_TRUE(portWrite[cs1][0] == 0.0);
    TEST_ASSERT_TRUE(portWrite[cs1][1] == 1.0);
}

// [test->fw~hal_spi_004~1]
static void test_cs_active_high_polarity(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
    const int32_t cs2 = portHandle("CS2");
    TEST_ASSERT_TRUE(cs2 >= 0);
    clearPortWrites();

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_2, tx, 1U));
    // Active-high: the inverse of active-low.
    TEST_ASSERT_EQUAL_UINT32(2U, portWrites[cs2]);
    TEST_ASSERT_TRUE(portWrite[cs2][0] == 1.0);
    TEST_ASSERT_TRUE(portWrite[cs2][1] == 0.0);
}

// [test->fw~hal_spi_004~1]
static void test_cs_hw_mode_drives_no_gpio(void)
{
    spiChannels[HW_SPI_CHANNEL_AS5048_1].csMode = HW_SPI_CS_MODE_HW;
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
    const int32_t cs1 = portHandle("CS1");
    TEST_ASSERT_TRUE(cs1 >= 0);
    clearPortWrites();

    uint8_t tx[1] = { 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_AS5048_1, tx, 1U));
    // Hardware NSS: the peripheral owns the line, so no GPIO pin moves.
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[cs1]);
}

// [test->fw~hal_spi_004~1]
static void test_cs_none_mode_drives_no_gpio(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
    const int32_t cs1 = portHandle("CS1");
    const int32_t cs2 = portHandle("CS2");
    TEST_ASSERT_TRUE((cs1 >= 0) && (cs2 >= 0));
    clearPortWrites();

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    pendedHandler();
    // The CS-less device drives no chip-select at all.
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[cs1]);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[cs2]);
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

    // Returns immediately; the completion interrupt is pended.
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(1U, pendCalls);
    TEST_ASSERT_EQUAL_INT32(pendedRegisterReturn, lastPendHandle);
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount);

    pendedHandler();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_SPI_CHANNEL_SK6805_STRING, cbChannel);
    TEST_ASSERT_EQUAL_PTR(&ctx, cbContext);

    // Callback fires exactly once.
    pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
}

// [test->fw~hal_spi_005~1]
static void test_async_observable_by_polling_only(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_BUSY, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));

    pendedHandler();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount); // no callback registered
}

// [test->fw~hal_spi_005~1]
static void test_reinit_rewires_completion_and_clears_callback(void)
{
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
    TEST_ASSERT_TRUE(HW_SPI_registerCallback(HW_SPI_CHANNEL_SK6805_STRING, testCallback, NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, pendedRegisterCalls);

    // Re-init: the old completion IRQ is cancelled, a fresh one registered,
    // and the callback slot is cleared.
    TEST_ASSERT_TRUE(HW_SPI_init(&spiConfig));
    TEST_ASSERT_EQUAL_UINT32(1U, cancelCalls);
    TEST_ASSERT_EQUAL_INT32(pendedRegisterReturn, lastCancelHandle);
    TEST_ASSERT_EQUAL_UINT32(2U, pendedRegisterCalls);

    uint8_t tx[2] = { 0U, 0U };
    TEST_ASSERT_TRUE(HW_SPI_transmit(HW_SPI_CHANNEL_SK6805_STRING, tx, 2U));
    pendedHandler();
    TEST_ASSERT_EQUAL_INT(HW_SPI_STATUS_COMPLETE, HW_SPI_getStatus(HW_SPI_CHANNEL_SK6805_STRING));
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount);
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
    pendedHandler();
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
    pendedHandler();
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

    RUN_TEST(test_receive_returns_linked_peer_frame);
    RUN_TEST(test_transmitReceive_unlinked_bus_reads_ones);
    RUN_TEST(test_timeout_formula);

    RUN_TEST(test_cs_active_low_polarity);
    RUN_TEST(test_cs_active_high_polarity);
    RUN_TEST(test_cs_hw_mode_drives_no_gpio);
    RUN_TEST(test_cs_none_mode_drives_no_gpio);

    RUN_TEST(test_async_busy_then_complete_with_callback);
    RUN_TEST(test_async_observable_by_polling_only);
    RUN_TEST(test_reinit_rewires_completion_and_clears_callback);

    RUN_TEST(test_mode_software_completes);
    RUN_TEST(test_mode_dma_completes);
    RUN_TEST(test_mode_interrupt_completes);

    return UNITY_END();
}
