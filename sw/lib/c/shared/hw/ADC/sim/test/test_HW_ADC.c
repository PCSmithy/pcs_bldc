#include "HW_ADC.h"
#include "HW_TIM.h"
#include "SIL_irq.h"
#include "SIL_irq_double.h"
#include "unity.h"

#define VREF        (3.3f)
#define NUM_BITS    (12U)
#define MAX_COUNTS  (4095.0f)   // 2^NUM_BITS - 1

// Local TIM fixture for the triggered path: center counter, 2000 µs cycle,
// OC-match trigger on unit 0 — up-cross lands at 400 µs, down-cross at 1600.
#define TIM_PERIOD     (1000U)
#define TIM_COMPARE    (400U)
#define UP_CROSS_US    (400U)
#define DOWN_CROSS_US  (1200U)   // from the up-cross: 400 -> 1600

// File-scope config the tests build and tweak; HW_ADC_init stores a pointer
// to it, so it must outlive each test.
static HW_ADC_channelConfig_S adcChannels[HW_ADC_CHANNEL_COUNT];
static HW_ADC_config_S        adcConfig;

static HW_TIM_peripheralConfig_S timPeripherals[HW_TIM_PERIPHERAL_COUNT];
static HW_TIM_channelConfig_S    timChannels[HW_TIM_CHANNEL_COUNT];
static HW_TIM_config_S           timConfig;


// Counting injected callbacks + the status each was handed.
static uint32_t cbACalls;
static uint32_t cbBCalls;
static HW_ADC_conversionStatus_E cbLastStatus;

static void injectedCbA(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E status, void * context)
{
    (void)channel; (void)context;
    cbACalls++;
    cbLastStatus = status;
}

static void injectedCbB(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E status, void * context)
{
    (void)channel; (void)context;
    cbBCalls++;
    cbLastStatus = status;
}

// Good baseline: two channels, software + polled on both sequences. Channel 1
// has regular inputs 3 and 7 and injected slots 0 and 1 enabled; channel 2 has
// regular input 0.
static void buildGoodConfig(void)
{
    for (size_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
    {
        adcChannels[ch] = (HW_ADC_channelConfig_S){
            .triggerMode         = HW_ADC_TRIGGER_SOFTWARE,
            .xferMode            = HW_ADC_XFER_POLLED,
            .injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE,
            .injectedXferMode    = HW_ADC_XFER_POLLED,
            .vref                = VREF,
            .numBits             = NUM_BITS,
            .configureMultimode  = false };
    }
    adcChannels[HW_ADC_CHANNEL_1].inputs[3].enabled         = true;
    adcChannels[HW_ADC_CHANNEL_1].inputs[7].enabled         = true;
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[0].enabled = true;
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[1].enabled = true;
    adcChannels[HW_ADC_CHANNEL_2].inputs[0].enabled         = true;

    adcConfig = (HW_ADC_config_S){
        .channels = adcChannels, .numChannels = HW_ADC_CHANNEL_COUNT };
}

// Channel 1 rewired to the timer-triggered interrupt path: one injected slot
// sampling pin 3 (shared with the enabled regular input), rising edge.
static void makeChannel1Triggered(void)
{
    adcChannels[HW_ADC_CHANNEL_1].injectedTriggerMode         = HW_ADC_TRIGGER_TIMER;
    adcChannels[HW_ADC_CHANNEL_1].injectedXferMode            = HW_ADC_XFER_INTERRUPT;
    adcChannels[HW_ADC_CHANNEL_1].injectedTimerTrigger        = HW_ADC_TIMER_TRIGGER_PWM_TIM_TRGO;
    adcChannels[HW_ADC_CHANNEL_1].injectedTriggerEdge         = HW_ADC_TRIGGER_EDGE_RISING;
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[0].pinInput  = 3U;
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[1].enabled   = false;
}

static void buildTimConfig(void)
{
    for (size_t p = 0U; p < HW_TIM_PERIPHERAL_COUNT; p++)
    {
        timPeripherals[p] = (HW_TIM_peripheralConfig_S){ 0 };
    }
    timPeripherals[HW_TIM_PERIPHERAL_1] = (HW_TIM_peripheralConfig_S){
        .nameStr          = "TIM1",
        .period           = TIM_PERIOD,
        .counterWidthBits = 16U,
        .countDir         = HW_TIM_COUNT_CENTER,
        .countsPerUs      = 1U,
        .configureTrgo    = true,
        .trgoSource       = HW_TIM_TRGO_OC_MATCH,
        .trgoOcUnit       = 0U };
    timPeripherals[HW_TIM_PERIPHERAL_2] = (HW_TIM_peripheralConfig_S){
        .nameStr          = "TIM2",
        .period           = 0xFFFFFFFFU,
        .counterWidthBits = 32U,
        .countDir         = HW_TIM_COUNT_UP };

    for (size_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        timChannels[ch] = (HW_TIM_channelConfig_S){
            .peripheral = HW_TIM_PERIPHERAL_1,
            .role       = HW_TIM_ROLE_OUTPUT_COMPARE,
            .ocUnit     = (uint8_t)ch,
            .compare    = ((ch == 0U) ? TIM_COMPARE : 0U) };
    }

    timConfig = (HW_TIM_config_S){
        .peripherals    = timPeripherals,
        .numPeripherals = HW_TIM_PERIPHERAL_COUNT,
        .channels       = timChannels,
        .numChannels    = HW_TIM_CHANNEL_COUNT };
}

// Arm the triggered engine end to end: irq double installed before init (the
// completion registers there), TIM re-seeded after (the trgo sink survives).
static void armTriggeredEngine(void)
{
    SIL_irq_double_install(11);
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    buildTimConfig();
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
}

void setUp(void)
{
    // A rejected init is the clean slate: init drops the driver to its
    // uninitialized state before it looks at the config.
    // Install clears the double's log; the hooks go back on in armTriggeredEngine.
    SIL_irq_double_install(11);
    SIL_irq_setHooks(NULL);
    (void)HW_ADC_init(NULL);
    buildGoodConfig();

    cbACalls             = 0U;
    cbBCalls             = 0U;
    cbLastStatus         = HW_ADC_CONVERSION_STATUS_IDLE;
}

void tearDown(void)
{
    SIL_irq_setHooks(NULL);
}

/* ---- fw~hal_adc_001: init + config validation ---- */
// [test->fw~hal_adc_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
}

// [test->fw~hal_adc_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_ADC_init(NULL));
}

// [test->fw~hal_adc_001~1]
static void test_init_null_channels(void)
{
    adcConfig.channels = NULL;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));
}

// [test->fw~hal_adc_001~1]
static void test_init_too_many_channels(void)
{
    adcConfig.numChannels = HW_ADC_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));
}

// [test->fw~hal_adc_001~1]
static void test_init_rejects_injected_gap(void)
{
    // Enabled slots must be contiguous from 0; [0]=on,[1]=off,[2]=on is a gap.
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[1].enabled = false;
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[2].enabled = true;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));
}

/* ---- fw~hal_adc_004: software+polled init + sample smoke ---- */
// Only software-triggered + polled is built; the full mode taxonomy
// (fw~hal_adc_003) stays unimplemented and OFT-uncovered until the timer,
// interrupt, and DMA paths land.
// [test->fw~hal_adc_004~1]
static void test_software_polled_initializes_and_samples(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));

    // No pass has run yet: status reports IDLE.
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_FAULT;
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_IDLE, status);

    HW_ADC_run1ms();
    uint32_t count = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, status);
}

/* ---- fw~hal_adc_002: regular-sequence channel/input addressing ---- */
// [test->fw~hal_adc_002~1]
static void test_only_enabled_inputs_convert(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    HW_ADC_run1ms();

    uint32_t count = 0U;
    // Enabled inputs on channel 1 are readable...
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 7U, &count));
    // ...a disabled input on the same channel is not.
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 5U, &count));
}

// [test->fw~hal_adc_002~1]
static void test_channels_addressed_independently(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    HW_ADC_run1ms();

    uint32_t c1 = 0U;
    uint32_t c2 = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &c1));
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_2, 0U, &c2));
    // Channel 2's input 3 is not enabled; channel 1's input 0 is not.
    uint32_t scratch = 0U;
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_2, 3U, &scratch));
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 0U, &scratch));
}

/* ---- fw~hal_adc_004: polled software-triggered sampling ---- */
// [test->fw~hal_adc_004~1]
static void test_sampling_before_init_is_noop(void)
{
    // No init: a sampling pass does nothing and reads fail.
    HW_ADC_run1ms();
    uint32_t count = 0U;
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
}

// [test->fw~hal_adc_004~1]
static void test_status_failure_modes(void)
{
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_OK;

    // Before init: fails.
    TEST_ASSERT_FALSE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));

    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));

    // Out-of-range channel and NULL destination both fail.
    TEST_ASSERT_FALSE(HW_ADC_getStatus(HW_ADC_CHANNEL_COUNT, &status));
    TEST_ASSERT_FALSE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, NULL));
}

/* ---- fw~hal_adc_005: counts + volts readout ---- */
// [test->fw~hal_adc_005~1]
static void test_readout_failure_modes(void)
{
    uint32_t count = 0U;

    // Before init: fails.
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));

    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    HW_ADC_run1ms();

    // Disabled input, out-of-range index, and NULL destination all fail.
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 5U, &count));
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, HW_ADC_INPUTS_PER_CHANNEL, &count));
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, NULL));
    TEST_ASSERT_FALSE(HW_ADC_getVolts(HW_ADC_CHANNEL_1, 3U, NULL));
}

/* ---- fw~hal_adc_006: injected conversion sequence ---- */
// [test->fw~hal_adc_006~1]
static void test_injected_sampling_and_readout(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    HW_ADC_run1ms();

    uint32_t  count = 0U;
    float32_t volts = 0.0f;
    // Enabled injected positions 0 and 1 are readable.
    TEST_ASSERT_TRUE(HW_ADC_getInjectedCount(HW_ADC_CHANNEL_1, 0U, &count));
    TEST_ASSERT_TRUE(HW_ADC_getInjectedVolts(HW_ADC_CHANNEL_1, 1U, &volts));
    // A disabled position fails.
    TEST_ASSERT_FALSE(HW_ADC_getInjectedCount(HW_ADC_CHANNEL_1, 2U, &count));

    // Volts use the same formula as the regular path.
    TEST_ASSERT_TRUE(HW_ADC_getInjectedCount(HW_ADC_CHANNEL_1, 1U, &count));
    const float32_t expected = ((float32_t)count / MAX_COUNTS) * VREF;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected, volts);
}

/* ---- fw~hal_adc_003: timer-triggered injected — config validation ---- */
// [test->fw~hal_adc_001~1]
static void test_init_rejects_unbuilt_injected_mode_combos(void)
{
    adcChannels[HW_ADC_CHANNEL_1].injectedTriggerMode = HW_ADC_TRIGGER_TIMER;
    adcChannels[HW_ADC_CHANNEL_1].injectedXferMode    = HW_ADC_XFER_POLLED;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));

    adcChannels[HW_ADC_CHANNEL_1].injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE;
    adcChannels[HW_ADC_CHANNEL_1].injectedXferMode    = HW_ADC_XFER_INTERRUPT;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));
}

// [test->fw~hal_adc_001~1]
static void test_init_rejects_out_of_range_injected_pin(void)
{
    makeChannel1Triggered();
    adcChannels[HW_ADC_CHANNEL_1].injectedInputs[0].pinInput = HW_ADC_INPUTS_PER_CHANNEL;
    TEST_ASSERT_FALSE(HW_ADC_init(&adcConfig));
}

/* ---- fw~hal_adc_003 / _008: timer-triggered injected — engine behavior ---- */
// [test->fw~hal_adc_008~1]
static void test_injected_status_walk_and_guards(void)
{
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_FAULT;

    // Before init: fails.
    TEST_ASSERT_FALSE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_1, &status));

    makeChannel1Triggered();
    armTriggeredEngine();

    // Armed channel is BUSY until the first completion; the polled neighbor
    // stays IDLE. Out-of-range channel and NULL destination fail.
    TEST_ASSERT_TRUE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_BUSY, status);
    TEST_ASSERT_TRUE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_2, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_IDLE, status);
    TEST_ASSERT_FALSE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_COUNT, &status));
    TEST_ASSERT_FALSE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_1, NULL));
}

// [test->fw~hal_adc_003~1]
static void test_falling_edge_selects_the_down_crossing(void)
{
    makeChannel1Triggered();
    adcChannels[HW_ADC_CHANNEL_1].injectedTriggerEdge = HW_ADC_TRIGGER_EDGE_FALLING;
    armTriggeredEngine();
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.pendedRegisterCalls);

    // The up-count crossing is the rejected edge: nothing pends.
    HW_TIM_advanceTime(UP_CROSS_US);
    TEST_ASSERT_EQUAL_UINT32(0U, SIL_irq_double.pendCalls);

    // The down-count crossing pends the completion; running it lands OK.
    HW_TIM_advanceTime(DOWN_CROSS_US);
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.pendCalls);
    TEST_ASSERT_EQUAL_INT32(SIL_irq_double.pendedRegisterReturn, SIL_irq_double.lastPendHandle);
    SIL_irq_double.pendedHandler();
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_IDLE;
    TEST_ASSERT_TRUE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, status);
}

// [test->fw~hal_adc_008~1]
static void test_injected_callback_last_wins_and_null_unregisters(void)
{
    makeChannel1Triggered();
    armTriggeredEngine();

    // Both registered: the later registration owns the slot.
    TEST_ASSERT_TRUE(HW_ADC_registerInjectedCallback(HW_ADC_CHANNEL_1, injectedCbA, NULL));
    TEST_ASSERT_TRUE(HW_ADC_registerInjectedCallback(HW_ADC_CHANNEL_1, injectedCbB, NULL));
    HW_TIM_advanceTime(UP_CROSS_US);
    SIL_irq_double.pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(0U, cbACalls);
    TEST_ASSERT_EQUAL_UINT32(1U, cbBCalls);
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, cbLastStatus);

    // NULL unregisters: the next completion invokes nobody, status still walks.
    TEST_ASSERT_TRUE(HW_ADC_registerInjectedCallback(HW_ADC_CHANNEL_1, NULL, NULL));
    HW_TIM_advanceTime(2U * TIM_PERIOD);
    SIL_irq_double.pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(1U, cbBCalls);
}

// [test->fw~hal_adc_008~1]
static void test_reinit_rewires_completion_and_clears_callback(void)
{
    makeChannel1Triggered();
    armTriggeredEngine();
    TEST_ASSERT_TRUE(HW_ADC_registerInjectedCallback(HW_ADC_CHANNEL_1, injectedCbA, NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.pendedRegisterCalls);

    // Re-init: the old completion IRQ is cancelled, a fresh one registered,
    // and the callback slot is cleared (G4 contract parity).
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.cancelCalls);
    TEST_ASSERT_EQUAL_INT32(SIL_irq_double.pendedRegisterReturn, SIL_irq_double.lastCancelHandle);
    TEST_ASSERT_EQUAL_UINT32(2U, SIL_irq_double.pendedRegisterCalls);

    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    HW_TIM_advanceTime(UP_CROSS_US);
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.pendCalls);
    SIL_irq_double.pendedHandler();
    TEST_ASSERT_EQUAL_UINT32(0U, cbACalls);

    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_IDLE;
    TEST_ASSERT_TRUE(HW_ADC_getInjectedStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, status);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_rejects_injected_gap);

    RUN_TEST(test_software_polled_initializes_and_samples);

    RUN_TEST(test_only_enabled_inputs_convert);
    RUN_TEST(test_channels_addressed_independently);

    RUN_TEST(test_sampling_before_init_is_noop);
    RUN_TEST(test_status_failure_modes);

    RUN_TEST(test_readout_failure_modes);

    RUN_TEST(test_injected_sampling_and_readout);

    RUN_TEST(test_init_rejects_unbuilt_injected_mode_combos);
    RUN_TEST(test_init_rejects_out_of_range_injected_pin);
    RUN_TEST(test_injected_status_walk_and_guards);
    RUN_TEST(test_falling_edge_selects_the_down_crossing);
    RUN_TEST(test_injected_callback_last_wins_and_null_unregisters);
    RUN_TEST(test_reinit_rewires_completion_and_clears_callback);

    return UNITY_END();
}
