#include "HW_OPAMP.h"
#include "unity.h"

// File-scope config the tests build and tweak before each init call. The
// gain-math behavior lives in the SIL suite (opamp_behavior.rs), which drives
// the driver's DWARF-visible statics; Unity covers the init contract.
static HW_OPAMP_channelConfig_S opampChannels[HW_OPAMP_CHANNEL_COUNT];
static HW_OPAMP_config_S        opampConfig;

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

// Init is re-entrant: a rejected config between two good ones changes nothing.
// [test->fw~hal_opamp_001~1]
static void test_init_reentrant(void)
{
    TEST_ASSERT_TRUE(HW_OPAMP_init(&opampConfig));
    TEST_ASSERT_FALSE(HW_OPAMP_init(NULL));
    TEST_ASSERT_TRUE(HW_OPAMP_init(&opampConfig));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_too_many_channels);
    RUN_TEST(test_init_reentrant);

    return UNITY_END();
}
