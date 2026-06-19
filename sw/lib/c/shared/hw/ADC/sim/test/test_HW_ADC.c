#include "HW_ADC.h"
#include "HW_ADC_sim.h"
#include "unity.h"

#define VREF        (3.3f)
#define NUM_BITS    (12U)
#define MAX_COUNTS  (4095.0f)   // 2^NUM_BITS - 1

// File-scope config the tests build and tweak; HW_ADC_init stores a pointer
// to it, so it must outlive each test.
static HW_ADC_channelConfig_S adcChannels[HW_ADC_CHANNEL_COUNT];
static HW_ADC_config_S        adcConfig;

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

void setUp(void)
{
    HW_ADC_sim_reset();
    buildGoodConfig();
}

void tearDown(void) {}

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
    HW_ADC_run1ms();
    uint32_t count = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
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
static void test_sampling_pass_updates_counts(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));

    HW_ADC_run1ms();
    uint32_t after = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &after));

    // The sim ramps each pass, so a second pass updates the stored value.
    HW_ADC_run1ms();
    uint32_t after2 = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &after2));
    TEST_ASSERT_TRUE(after != after2);
}

// [test->fw~hal_adc_004~1]
static void test_sampling_before_init_is_noop(void)
{
    // No init: a sampling pass does nothing and reads fail.
    HW_ADC_run1ms();
    uint32_t count = 0U;
    TEST_ASSERT_FALSE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
}

// [test->fw~hal_adc_004~1]
static void test_conversion_fault_status_and_count_retained(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));

    // A read before any pass: status reports IDLE.
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_FAULT;
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_IDLE, status);

    // A clean pass: status OK, count populated.
    HW_ADC_run1ms();
    uint32_t good = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &good));
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, status);

    // Inject a stalled conversion: status FAULT, prior count retained.
    HW_ADC_sim_setConversionStall(HW_ADC_CHANNEL_1, true);
    HW_ADC_run1ms();
    uint32_t afterStall = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &afterStall));
    TEST_ASSERT_EQUAL_UINT32(good, afterStall);
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_FAULT, status);

    // Clearing the stall resumes sampling: status OK, count advances.
    HW_ADC_sim_setConversionStall(HW_ADC_CHANNEL_1, false);
    HW_ADC_run1ms();
    uint32_t recovered = 0U;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &recovered));
    TEST_ASSERT_TRUE(recovered != afterStall);
    TEST_ASSERT_TRUE(HW_ADC_getStatus(HW_ADC_CHANNEL_1, &status));
    TEST_ASSERT_EQUAL(HW_ADC_CONVERSION_STATUS_OK, status);
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
static void test_volts_matches_formula(void)
{
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));
    HW_ADC_run1ms();

    uint32_t  count = 0U;
    float32_t volts = 0.0f;
    TEST_ASSERT_TRUE(HW_ADC_getCount(HW_ADC_CHANNEL_1, 3U, &count));
    TEST_ASSERT_TRUE(HW_ADC_getVolts(HW_ADC_CHANNEL_1, 3U, &volts));

    const float32_t expected = ((float32_t)count / MAX_COUNTS) * VREF;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected, volts);
}

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

/* ---- fw~hal_adc_007: dual-ADC multimode configuration ---- */
// [test->fw~hal_adc_007~1]
static void test_multimode_applied_to_master_only(void)
{
    adcChannels[HW_ADC_CHANNEL_1].configureMultimode = true;   // master
    adcChannels[HW_ADC_CHANNEL_2].configureMultimode = false;  // slave
    TEST_ASSERT_TRUE(HW_ADC_init(&adcConfig));

    TEST_ASSERT_TRUE(HW_ADC_sim_getMultimodeApplied(HW_ADC_CHANNEL_1));
    TEST_ASSERT_FALSE(HW_ADC_sim_getMultimodeApplied(HW_ADC_CHANNEL_2));
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

    RUN_TEST(test_sampling_pass_updates_counts);
    RUN_TEST(test_sampling_before_init_is_noop);
    RUN_TEST(test_conversion_fault_status_and_count_retained);
    RUN_TEST(test_status_failure_modes);

    RUN_TEST(test_volts_matches_formula);
    RUN_TEST(test_readout_failure_modes);

    RUN_TEST(test_injected_sampling_and_readout);

    RUN_TEST(test_multimode_applied_to_master_only);

    return UNITY_END();
}
