#include "HW_TIM.h"
#include "SIL_ports.h"
#include "unity.h"

#include <string.h>

// Peripheral 1 is the advanced-control PWM timer carrying three complementary
// output-compare channels + a break input (hence an MOE latch); peripheral 2 is
// a wide free-running time base. Tests tweak the file-scope config then call
// HW_TIM_init, so it must outlive each test. HW_TIM_init fully re-seeds the
// driver, so each test starts from a clean state by initializing.
#define PWM_PERIOD     (1000U)
#define PWM_COMPARE    (400U)
#define PWM_DEADTIME   (50U)

#define PWM_PERIPH     (HW_TIM_PERIPHERAL_1)
#define BASE_PERIPH    (HW_TIM_PERIPHERAL_2)
#define PWM_CH         (HW_TIM_CHANNEL_PWM_U)

// Test-owned SIL_ports hooks double: registerSignal assigns sequential handles
// and remembers each port's local name; writeSignal records the last value per
// handle. Installed before HW_TIM_init so the driver's port registrations and
// publications run through the production seam. With no hooks installed the
// driver registers nothing and its writes no-op (the standalone-native contract).
#define MAX_PORTS  (16)
static char     portName[MAX_PORTS][40];
static double   portValue[MAX_PORTS];
static bool     portWritten[MAX_PORTS];
static int32_t  portCount;

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
        portValue[handle]   = value;
        portWritten[handle] = true;
    }
}

static void installPortsDouble(void)
{
    const SIL_ports_hooks_S hooks = {
        .context        = NULL,
        .registerSignal = hookRegister,
        .readSignal     = NULL,
        .writeSignal    = hookWrite,
        .duplexTransfer = NULL,
    };
    SIL_ports_setHooks(&hooks);
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

static HW_TIM_peripheralConfig_S timPeripherals[HW_TIM_PERIPHERAL_COUNT];
static HW_TIM_channelConfig_S    timChannels[HW_TIM_CHANNEL_COUNT];
static HW_TIM_config_S           timConfig;

static void buildGoodConfig(void)
{
    for (size_t p = 0U; p < HW_TIM_PERIPHERAL_COUNT; p++)
    {
        timPeripherals[p] = (HW_TIM_peripheralConfig_S){ 0 };
    }
    for (size_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        timChannels[ch] = (HW_TIM_channelConfig_S){ 0 };
    }

    timPeripherals[PWM_PERIPH] = (HW_TIM_peripheralConfig_S){
        .nameStr          = "TIM1",
        .prescaler        = 0U,
        .period           = PWM_PERIOD,
        .counterWidthBits = 16U,
        .countDir         = HW_TIM_COUNT_UP,
        .configureBreakDeadTime = true,
        .deadTime               = PWM_DEADTIME,
        .hasBreakInput          = true,
        .configureTrgo          = true,
        .trgoSource             = HW_TIM_TRGO_UPDATE };

    timPeripherals[BASE_PERIPH] = (HW_TIM_peripheralConfig_S){
        .nameStr          = "TIM2",
        .prescaler        = 143U,
        .period           = 0xFFFFFFFFU,
        .counterWidthBits = 32U,
        .countDir         = HW_TIM_COUNT_UP };

    // Three complementary output-compare channels on the PWM peripheral.
    timChannels[HW_TIM_CHANNEL_PWM_U] = (HW_TIM_channelConfig_S){
        .peripheral = PWM_PERIPH, .role = HW_TIM_ROLE_OUTPUT_COMPARE, .ocUnit = 0U,
        .complementary = true, .compare = PWM_COMPARE, .inactiveLevel = 0U,
        .channelNameStr = "PWM_U" };
    timChannels[HW_TIM_CHANNEL_PWM_V] = (HW_TIM_channelConfig_S){
        .peripheral = PWM_PERIPH, .role = HW_TIM_ROLE_OUTPUT_COMPARE, .ocUnit = 1U,
        .complementary = true, .compare = PWM_COMPARE, .inactiveLevel = 0U,
        .channelNameStr = "PWM_V" };
    timChannels[HW_TIM_CHANNEL_PWM_W] = (HW_TIM_channelConfig_S){
        .peripheral = PWM_PERIPH, .role = HW_TIM_ROLE_OUTPUT_COMPARE, .ocUnit = 2U,
        .complementary = true, .compare = PWM_COMPARE, .inactiveLevel = 0U,
        .channelNameStr = "PWM_W" };

    timConfig = (HW_TIM_config_S){
        .peripherals    = timPeripherals,
        .numPeripherals = HW_TIM_PERIPHERAL_COUNT,
        .channels       = timChannels,
        .numChannels    = HW_TIM_CHANNEL_COUNT };
}

void setUp(void)
{
    SIL_ports_setHooks(NULL); // unlinked by default; port tests opt in
    for (int32_t i = 0; i < MAX_PORTS; i++)
    {
        portName[i][0] = '\0';
        portValue[i]   = 0.0;
        portWritten[i] = false;
    }
    portCount = 0;
    buildGoodConfig();
}

void tearDown(void)
{
    SIL_ports_setHooks(NULL);
}

/* ---- fw~hal_tim_001: init + config validation ---- */
// [test->fw~hal_tim_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_TIM_init(NULL));
}

// [test->fw~hal_tim_001~1]
static void test_init_null_peripherals(void)
{
    timConfig.peripherals = NULL;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_null_channels(void)
{
    timConfig.channels = NULL;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_bad_count_direction(void)
{
    timPeripherals[PWM_PERIPH].countDir = (HW_TIM_countDir_E)99;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_compare_above_period(void)
{
    timChannels[0].compare = PWM_PERIOD + 1U;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_period_above_width(void)
{
    timPeripherals[PWM_PERIPH].period = 0x10000U;   // exceeds 16-bit width
    timChannels[0].compare = 0U;                    // keep compare <= period
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_oversized_deadtime(void)
{
    timPeripherals[PWM_PERIPH].deadTime = 300U;     // dead-time generator is 8 bits
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_unsupported_role(void)
{
    timChannels[0].role = (HW_TIM_channelRole_E)99;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_channel_bad_peripheral(void)
{
    timChannels[0].peripheral = HW_TIM_PERIPHERAL_COUNT;   // not a configured peripheral
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

// [test->fw~hal_tim_001~1]
static void test_init_rejects_bad_ocUnit(void)
{
    timChannels[0].ocUnit = HW_TIM_OC_UNITS_PER_PERIPHERAL;
    TEST_ASSERT_FALSE(HW_TIM_init(&timConfig));
}

/* ---- fw~hal_tim_003: free-running counter readout ---- */
// [test->fw~hal_tim_003~1]
static void test_getCounter_out_of_range(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    uint32_t counter = 0U;
    TEST_ASSERT_FALSE(HW_TIM_getCounter(HW_TIM_PERIPHERAL_COUNT, &counter));
}

// [test->fw~hal_tim_003~1]
static void test_getCounter_null_out(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_getCounter(PWM_PERIPH, NULL));
}

// [test->fw~hal_tim_003~1]
static void test_getPeriod_and_peripheral(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    uint32_t period = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getPeriod(PWM_CH, &period));
    TEST_ASSERT_EQUAL_UINT32(PWM_PERIOD, period);

    HW_TIM_peripheral_E periph = BASE_PERIPH;
    TEST_ASSERT_TRUE(HW_TIM_getPeripheral(PWM_CH, &periph));
    TEST_ASSERT_EQUAL_INT(PWM_PERIPH, periph);
}

/* ---- fw~hal_tim_004: output-compare unit operation ---- */
// [test->fw~hal_tim_004~1]
static void test_setCompare_readback(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_setCompare(PWM_CH, 250U));

    uint32_t compare = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCompare(PWM_CH, &compare));
    TEST_ASSERT_EQUAL_UINT32(250U, compare);
}

// [test->fw~hal_tim_004~1]
static void test_setCompare_above_period_rejected(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_setCompare(PWM_CH, PWM_PERIOD + 1U));
}

// [test->fw~hal_tim_004~1]
static void test_setCompare_out_of_range_channel_rejected(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_FALSE(HW_TIM_setCompare(HW_TIM_CHANNEL_COUNT, 100U));
}

/* ---- fw~hal_tim_008: master output enable ---- */
// [test->fw~hal_tim_008~1]
static void test_moe_set_get(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    // Commanded OFF at init.
    bool moe = true;
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_PERIPH, &moe));
    TEST_ASSERT_FALSE(moe);

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_PERIPH, true));
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_PERIPH, &moe));
    TEST_ASSERT_TRUE(moe);

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_PERIPH, false));
    TEST_ASSERT_TRUE(HW_TIM_getMainOutputEnabled(PWM_PERIPH, &moe));
    TEST_ASSERT_FALSE(moe);
}

// [test->fw~hal_tim_008~1]
static void test_moe_error_returns(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    bool moe = false;
    // Out-of-range peripheral.
    TEST_ASSERT_FALSE(HW_TIM_setMainOutputEnabled(HW_TIM_PERIPHERAL_COUNT, true));
    TEST_ASSERT_FALSE(HW_TIM_getMainOutputEnabled(HW_TIM_PERIPHERAL_COUNT, &moe));
    // NULL out-pointer.
    TEST_ASSERT_FALSE(HW_TIM_getMainOutputEnabled(PWM_PERIPH, NULL));
}

// clearBreakFlags succeeds for a valid peripheral; firmware calls it to drop a
// power-up latch. MOE is untouched.
// [test->fw~hal_tim_008~1]
static void test_clearBreakFlags(void)
{
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_clearBreakFlags(PWM_PERIPH));
    TEST_ASSERT_FALSE(HW_TIM_clearBreakFlags(HW_TIM_PERIPHERAL_COUNT));
}

/* ---- fw~hal_tim_002: counter direction and period ---- */
// The sim models up- and down-counting; the spec's center-aligned mode has no
// sim counterpart, so these cover the up/down halves only.

// advanceTime drives only countsPerUs-configured counters: the timebase
// peripheral tracks elapsed sim time (wrapping modulo period+1); a
// countsPerUs=0 peripheral (the PWM carrier) stays put.
// [test->fw~hal_tim_002~1]
static void test_advanceTime_tracks_sim_time(void)
{
    timPeripherals[BASE_PERIPH].countsPerUs = 1U;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    HW_TIM_advanceTime(1000U);
    HW_TIM_advanceTime(1000U);

    uint32_t base = 0U;
    uint32_t pwm  = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_PERIPH, &pwm));
    TEST_ASSERT_EQUAL_UINT32(2000U, base);
    TEST_ASSERT_EQUAL_UINT32(0U, pwm); // countsPerUs = 0: untouched
}

// Wrap: a small-period counter advances modulo (period + 1).
// [test->fw~hal_tim_002~1]
static void test_advanceTime_wraps_at_period(void)
{
    timPeripherals[BASE_PERIPH].countsPerUs = 1U;
    timPeripherals[BASE_PERIPH].period = 99U;
    timPeripherals[BASE_PERIPH].counterWidthBits = 16U;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    HW_TIM_advanceTime(250U);

    uint32_t base = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(50U, base); // 250 mod 100
}

// Down-count: the counter seeds at the period and walks toward zero, wrapping
// back through the period on underflow.
// [test->fw~hal_tim_002~1]
static void test_advanceTime_counts_down(void)
{
    timPeripherals[BASE_PERIPH].countsPerUs = 1U;
    timPeripherals[BASE_PERIPH].period = 99U;
    timPeripherals[BASE_PERIPH].counterWidthBits = 16U;
    timPeripherals[BASE_PERIPH].countDir = HW_TIM_COUNT_DOWN;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    uint32_t base = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(99U, base); // seeded at the period

    HW_TIM_advanceTime(30U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(69U, base); // 99 - 30

    HW_TIM_advanceTime(90U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(79U, base); // 69 - 90 wraps through the period
}

/* ---- PWM/bridge observation ports ---- */

// Every named channel registers a duty + enable port, and the advanced-control
// peripheral registers one MOE port — seven in all.
static void test_ports_registered(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    TEST_ASSERT_EQUAL_INT32(7, portCount);
    const char * const expected[7] = {
        "PWM_U_duty", "PWM_U_enabled",
        "PWM_V_duty", "PWM_V_enabled",
        "PWM_W_duty", "PWM_W_enabled",
        "TIM1_MOE" };
    for (size_t i = 0U; i < 7U; i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(portHandle(expected[i]) >= 0, expected[i]);
    }
}

// A NULL channelNameStr registers no ports; the timebase (no break input) gets
// no MOE port.
static void test_unnamed_channel_registers_no_ports(void)
{
    installPortsDouble();
    timChannels[HW_TIM_CHANNEL_PWM_V].channelNameStr = NULL;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    TEST_ASSERT_TRUE(portHandle("PWM_V_duty") < 0);
    TEST_ASSERT_TRUE(portHandle("PWM_V_enabled") < 0);
    TEST_ASSERT_TRUE(portHandle("TIM2_MOE") < 0);
    TEST_ASSERT_EQUAL_INT32(5, portCount); // 2 named channels + 1 MOE
}

// Init publishes the dark-bridge boot state: every duty and enable at 0, MOE 0.
// The dark bridge seeds compare 0 (duty 0); enables and MOE start off.
static void test_boot_state_published_dark(void)
{
    installPortsDouble();
    for (size_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        timChannels[ch].compare = 0U;
    }
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    const char * const zeroPorts[7] = {
        "PWM_U_duty", "PWM_U_enabled",
        "PWM_V_duty", "PWM_V_enabled",
        "PWM_W_duty", "PWM_W_enabled",
        "TIM1_MOE" };
    for (size_t i = 0U; i < 7U; i++)
    {
        const int32_t h = portHandle(zeroPorts[i]);
        TEST_ASSERT_TRUE_MESSAGE(h >= 0, zeroPorts[i]);
        TEST_ASSERT_TRUE_MESSAGE(portWritten[h], zeroPorts[i]);
        TEST_ASSERT_TRUE_MESSAGE(portValue[h] == 0.0, zeroPorts[i]);
    }
}

// setCompare publishes the normalized duty (compare / period); raw counts never
// cross the boundary.
static void test_setCompare_publishes_normalized_duty(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    TEST_ASSERT_TRUE(HW_TIM_setCompare(PWM_CH, PWM_PERIOD / 4U)); // 0.25
    const int32_t h = portHandle("PWM_U_duty");
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_TRUE(portValue[h] == 0.25);
}

// setOutputEnabled publishes the per-phase enable as 0/1.
static void test_setOutputEnabled_publishes_enable(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, true));
    const int32_t h = portHandle("PWM_U_enabled");
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_TRUE(portValue[h] == 1.0);

    TEST_ASSERT_TRUE(HW_TIM_setOutputEnabled(PWM_CH, false));
    TEST_ASSERT_TRUE(portValue[h] == 0.0);
}

// setMainOutputEnabled publishes the master output enable as 0/1.
static void test_setMainOutputEnabled_publishes_moe(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_PERIPH, true));
    const int32_t h = portHandle("TIM1_MOE");
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_TRUE(portValue[h] == 1.0);

    TEST_ASSERT_TRUE(HW_TIM_setMainOutputEnabled(PWM_PERIPH, false));
    TEST_ASSERT_TRUE(portValue[h] == 0.0);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_peripherals);
    RUN_TEST(test_init_null_channels);
    RUN_TEST(test_init_rejects_bad_count_direction);
    RUN_TEST(test_init_rejects_compare_above_period);
    RUN_TEST(test_init_rejects_period_above_width);
    RUN_TEST(test_init_rejects_oversized_deadtime);
    RUN_TEST(test_init_rejects_unsupported_role);
    RUN_TEST(test_init_rejects_channel_bad_peripheral);
    RUN_TEST(test_init_rejects_bad_ocUnit);

    RUN_TEST(test_getCounter_out_of_range);
    RUN_TEST(test_getCounter_null_out);
    RUN_TEST(test_getPeriod_and_peripheral);

    RUN_TEST(test_setCompare_readback);
    RUN_TEST(test_setCompare_above_period_rejected);
    RUN_TEST(test_setCompare_out_of_range_channel_rejected);

    RUN_TEST(test_moe_set_get);
    RUN_TEST(test_moe_error_returns);
    RUN_TEST(test_clearBreakFlags);

    RUN_TEST(test_advanceTime_tracks_sim_time);
    RUN_TEST(test_advanceTime_wraps_at_period);
    RUN_TEST(test_advanceTime_counts_down);

    RUN_TEST(test_ports_registered);
    RUN_TEST(test_unnamed_channel_registers_no_ports);
    RUN_TEST(test_boot_state_published_dark);
    RUN_TEST(test_setCompare_publishes_normalized_duty);
    RUN_TEST(test_setOutputEnabled_publishes_enable);
    RUN_TEST(test_setMainOutputEnabled_publishes_moe);

    return UNITY_END();
}
