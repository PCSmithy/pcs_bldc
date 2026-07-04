#include "HW_OPAMP.h"
#include "HW_OPAMP_sim.h"
#include "unity.h"

// File-scope config the tests build and tweak; HW_OPAMP_init stores a
// pointer to it, so it must outlive each test.
static HW_OPAMP_channelConfig_S opampChannels[HW_OPAMP_CHANNEL_COUNT];
static HW_OPAMP_config_S        opampConfig;

// Good baseline: three amplifiers, each with a distinct gain so the
// amplification tests can tell them apart.
static void buildGoodConfig(void)
{
    opampChannels[HW_OPAMP_CHANNEL_1] =
        (HW_OPAMP_channelConfig_S){ .channelNameStr = "OPAMP1", .gain = 2.0f };
    opampChannels[HW_OPAMP_CHANNEL_2] =
        (HW_OPAMP_channelConfig_S){ .channelNameStr = "OPAMP2", .gain = 3.0f };
    opampChannels[HW_OPAMP_CHANNEL_3] =
        (HW_OPAMP_channelConfig_S){ .channelNameStr = "OPAMP3", .gain = 0.5f };

    opampConfig = (HW_OPAMP_config_S){
        .channels = opampChannels, .numChannels = HW_OPAMP_CHANNEL_COUNT };
}

void setUp(void)
{
    HW_OPAMP_sim_reset();
    buildGoodConfig();
}

void tearDown(void) {}

/* ---- fw~hal_opamp_001: init + config validation ---- */
// [test->fw~hal_opamp_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_OPAMP_init(&opampConfig));
}

// [test->fw~hal_opamp_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_OPAMP_init(NULL));
}

// [test->fw~hal_opamp_001~1]
static void test_init_null_channels(void)
{
    opampConfig.channels = NULL;
    TEST_ASSERT_FALSE(HW_OPAMP_init(&opampConfig));
}

// [test->fw~hal_opamp_001~1]
static void test_init_too_many_channels(void)
{
    opampConfig.numChannels = HW_OPAMP_CHANNEL_COUNT + 1U;
    TEST_ASSERT_FALSE(HW_OPAMP_init(&opampConfig));
}

/* ---- fw~hal_opamp_002: PGA amplification onto the internal ADC path ---- */
// [test->fw~hal_opamp_002~1]
static void test_output_is_input_times_gain(void)
{
    TEST_ASSERT_TRUE(HW_OPAMP_init(&opampConfig));

    HW_OPAMP_sim_setInputVolts(HW_OPAMP_CHANNEL_1, 0.5f);   // x2.0
    HW_OPAMP_sim_setInputVolts(HW_OPAMP_CHANNEL_2, 0.25f);  // x3.0
    HW_OPAMP_sim_setInputVolts(HW_OPAMP_CHANNEL_3, 1.2f);   // x0.5

    float32_t out = 0.0f;
    TEST_ASSERT_TRUE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_1, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out);
    TEST_ASSERT_TRUE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_2, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, out);
    TEST_ASSERT_TRUE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_3, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.6f, out);
}

// [test->fw~hal_opamp_002~1]
static void test_output_readout_failure_modes(void)
{
    float32_t out = 0.0f;

    // Before init: fails.
    TEST_ASSERT_FALSE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_1, &out));

    TEST_ASSERT_TRUE(HW_OPAMP_init(&opampConfig));

    // Out-of-range channel and NULL destination both fail.
    TEST_ASSERT_FALSE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_COUNT, &out));
    TEST_ASSERT_FALSE(HW_OPAMP_sim_getOutputVolts(HW_OPAMP_CHANNEL_1, NULL));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);

    RUN_TEST(test_output_is_input_times_gain);
    RUN_TEST(test_output_readout_failure_modes);

    return UNITY_END();
}
