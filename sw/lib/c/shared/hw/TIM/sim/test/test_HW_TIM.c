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

// Test-owned trigger sinks: recordTrgo counts invocations and remembers the
// peripheral + context it was handed; recordTrgoAlt is the second sink used to
// show which registration wins.
static uint32_t            trgoCount;
static uint32_t            trgoUpCount;
static uint32_t            trgoDownCount;
static HW_TIM_peripheral_E trgoPeripheral;
static void *              trgoContext;
static uint32_t            trgoAltCount;

static void recordTrgo(HW_TIM_peripheral_E peripheral, HW_TIM_trgoCross_E cross, void * context)
{
    trgoCount++;
    if (cross == HW_TIM_TRGO_CROSS_UP)
    {
        trgoUpCount++;
    }
    else
    {
        trgoDownCount++;
    }
    trgoPeripheral = peripheral;
    trgoContext    = context;
}

static void recordTrgoAlt(HW_TIM_peripheral_E peripheral, HW_TIM_trgoCross_E cross, void * context)
{
    (void)peripheral; (void)cross; (void)context;
    trgoAltCount++;
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

    // Non-complementary trigger-point channel on its own OC unit; unnamed so
    // the port set stays the three PWM pairs + MOE.
    timChannels[HW_TIM_CHANNEL_INJ_TRIG] = (HW_TIM_channelConfig_S){
        .peripheral = PWM_PERIPH, .role = HW_TIM_ROLE_OUTPUT_COMPARE, .ocUnit = 3U,
        .complementary = false, .compare = PWM_COMPARE, .inactiveLevel = 0U };

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

    // Sink registrations outlive HW_TIM_init, so each test starts from none.
    for (size_t p = 0U; p < HW_TIM_PERIPHERAL_COUNT; p++)
    {
        (void)HW_TIM_registerTrgoCallback((HW_TIM_peripheral_E)p, NULL, NULL);
    }
    trgoCount      = 0U;
    trgoUpCount    = 0U;
    trgoDownCount  = 0U;
    trgoAltCount   = 0U;
    trgoPeripheral = HW_TIM_PERIPHERAL_COUNT;
    trgoContext    = NULL;

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
// All three count directions are modelled: up, down, and center-aligned.

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

// Center-aligned: the counter walks up to the period and back down, so it reads
// out in both phases and a full cycle spans 2 x period counts.
// [test->fw~hal_tim_002~1]
static void test_advanceTime_counts_center_aligned(void)
{
    timPeripherals[BASE_PERIPH].countsPerUs = 1U;
    timPeripherals[BASE_PERIPH].period = 99U;
    timPeripherals[BASE_PERIPH].counterWidthBits = 16U;
    timPeripherals[BASE_PERIPH].countDir = HW_TIM_COUNT_CENTER;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    uint32_t base = 0xFFFFU;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(0U, base);   // seeded at the valley, counting up

    HW_TIM_advanceTime(40U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(40U, base);  // up phase

    HW_TIM_advanceTime(59U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(99U, base);  // the crest

    HW_TIM_advanceTime(30U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(69U, base);  // down phase: the same readout, falling

    HW_TIM_advanceTime(69U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(0U, base);   // 198 counts: one full cycle

    HW_TIM_advanceTime(198U);
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(0U, base);

    HW_TIM_advanceTime(298U);             // a cycle plus 100: one count past the crest
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(98U, base);
}

/* ---- fw~hal_tim_006: trigger output ---- */

// Set the timebase peripheral up as a short-period trigger source: 100 counts
// per lap at one count per microsecond.
static void buildTrgoTimebase(HW_TIM_trgoSource_E source)
{
    timPeripherals[BASE_PERIPH].period           = 99U;
    timPeripherals[BASE_PERIPH].counterWidthBits = 16U;
    timPeripherals[BASE_PERIPH].countsPerUs      = 1U;
    timPeripherals[BASE_PERIPH].configureTrgo    = true;
    timPeripherals[BASE_PERIPH].trgoSource       = source;
}

// An update source triggers at each period boundary, and the sink is handed the
// peripheral and the context it registered with. Registering ahead of
// HW_TIM_init works: the two are order-independent.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_fires_at_period_boundary(void)
{
    uint32_t ctx = 0U;
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, &ctx));

    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));

    HW_TIM_advanceTime(99U);    // one tick short of the boundary
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);
    TEST_ASSERT_EQUAL_INT(BASE_PERIPH, trgoPeripheral);
    TEST_ASSERT_EQUAL_PTR(&ctx, trgoContext);

    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);
}

// An advance spanning several periods triggers once per boundary it crosses.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_fires_per_crossing_in_one_advance(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(500U);   // five laps
    TEST_ASSERT_EQUAL_UINT32(5U, trgoCount);

    uint32_t base = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(BASE_PERIPH, &base));
    TEST_ASSERT_EQUAL_UINT32(0U, base);
}

// An advance that lands exactly on the boundary counts that landing once,
// whether it starts on the boundary value or short of it.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_counts_a_landing_once(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(100U);   // starts on the boundary value, one full lap
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(50U);
    HW_TIM_advanceTime(50U);    // the boundary falls at the end of this advance
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);
}

// A down-counter's update event is its reload, so the trigger lands on the
// underflow rather than on zero.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_on_down_counter(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].countDir = HW_TIM_COUNT_DOWN;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(99U);    // counter walks to zero
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(1U);     // underflow reloads the period
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(250U);   // two more reloads inside one advance
    TEST_ASSERT_EQUAL_UINT32(3U, trgoCount);
}

// A center-aligned counter reloads at both extremes, so with rcr = 0 — an
// update event per reload — the trigger lands twice per cycle.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_center_fires_at_both_extremes(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].countDir = HW_TIM_COUNT_CENTER;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(98U);    // one short of the crest
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(1U);     // the crest: overflow
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(99U);    // the valley: underflow, one full cycle done
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);

    HW_TIM_advanceTime(198U);   // another full cycle: both extremes again
    TEST_ASSERT_EQUAL_UINT32(4U, trgoCount);
}

// With rcr = 1 the countdown spends one reload and expires on the next, so a
// center-aligned counter started at the valley triggers once per cycle — at the
// valley, the crest having spent the countdown.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_center_with_rcr_fires_at_the_valley(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].countDir = HW_TIM_COUNT_CENTER;
    timPeripherals[BASE_PERIPH].rcr      = 1U;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(99U);    // the crest
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(99U);    // the valley
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(99U);    // the next crest
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(99U);    // and its valley
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);

    HW_TIM_advanceTime(594U);   // three full cycles
    TEST_ASSERT_EQUAL_UINT32(5U, trgoCount);
}

// The repetition counter thins an edge-aligned counter's updates too: one
// update event per rcr + 1 period boundaries.
// [test->fw~hal_tim_006~1]
static void test_trgo_update_honours_repetition_counter(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].rcr = 2U;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(100U);
    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(100U);   // the third boundary expires the countdown
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(600U);   // six more boundaries: two more updates
    TEST_ASSERT_EQUAL_UINT32(3U, trgoCount);
}

// An OC-match source triggers when the counter reaches the compare value.
// [test->fw~hal_tim_006~1]
static void test_trgo_oc_match_fires_at_compare(void)
{
    timPeripherals[PWM_PERIPH].countsPerUs = 1U;
    timPeripherals[PWM_PERIPH].trgoSource  = HW_TIM_TRGO_OC_MATCH;
    timPeripherals[PWM_PERIPH].trgoOcUnit  = 0U;   // PWM_U
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(PWM_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(PWM_COMPARE - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(PWM_PERIOD + 1U);   // one full lap: the same match again
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);
    TEST_ASSERT_EQUAL_UINT32(2U, trgoUpCount);   // up-counter: every event is an up-cross
}

// The trigger follows the configured OC unit's compare value, not another
// unit's.
// [test->fw~hal_tim_006~1]
static void test_trgo_oc_match_uses_configured_unit(void)
{
    timChannels[HW_TIM_CHANNEL_PWM_V].compare = 700U;
    timPeripherals[PWM_PERIPH].countsPerUs    = 1U;
    timPeripherals[PWM_PERIPH].trgoSource     = HW_TIM_TRGO_OC_MATCH;
    timPeripherals[PWM_PERIPH].trgoOcUnit     = 1U;   // PWM_V
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(PWM_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(PWM_COMPARE);   // unit 0's compare value
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    HW_TIM_advanceTime(300U);          // unit 1's
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);
}

// A center-aligned counter takes the compare value once per phase, so an
// OC-match source triggers on both crossings of a cycle.
// [test->fw~hal_tim_006~1]
static void test_trgo_oc_match_center_fires_on_both_crossings(void)
{
    timPeripherals[PWM_PERIPH].countsPerUs = 1U;
    timPeripherals[PWM_PERIPH].countDir    = HW_TIM_COUNT_CENTER;
    timPeripherals[PWM_PERIPH].trgoSource  = HW_TIM_TRGO_OC_MATCH;
    timPeripherals[PWM_PERIPH].trgoOcUnit  = 0U;   // PWM_U, compare 400 of period 1000
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(PWM_PERIPH, recordTrgo, NULL));

    HW_TIM_advanceTime(PWM_COMPARE);   // the up-phase crossing
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoUpCount);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoDownCount);

    HW_TIM_advanceTime(1199U);         // over the crest, one short of the way back down
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    HW_TIM_advanceTime(1U);            // the down-phase crossing
    TEST_ASSERT_EQUAL_UINT32(2U, trgoCount);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoUpCount);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoDownCount);

    uint32_t pwm = 0U;
    TEST_ASSERT_TRUE(HW_TIM_getCounter(PWM_PERIPH, &pwm));
    TEST_ASSERT_EQUAL_UINT32(PWM_COMPARE, pwm);

    HW_TIM_advanceTime(2U * PWM_PERIOD);   // one full cycle: both crossings again
    TEST_ASSERT_EQUAL_UINT32(4U, trgoCount);
}

// No trigger output configured, no source selected, or a counter that does not
// track sim time: nothing is emitted.
// [test->fw~hal_tim_006~1]
static void test_trgo_silent_when_not_configured(void)
{
    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].configureTrgo = false;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));
    HW_TIM_advanceTime(500U);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    buildTrgoTimebase(HW_TIM_TRGO_NONE);
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    HW_TIM_advanceTime(500U);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);

    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    timPeripherals[BASE_PERIPH].countsPerUs = 0U;
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    HW_TIM_advanceTime(500U);
    TEST_ASSERT_EQUAL_UINT32(0U, trgoCount);
}

// One sink per peripheral: a NULL callback clears it, a later registration
// replaces the earlier one, and HW_TIM_init leaves the registration standing.
// [test->fw~hal_tim_006~1]
static void test_trgo_registration(void)
{
    TEST_ASSERT_FALSE(HW_TIM_registerTrgoCallback(HW_TIM_PERIPHERAL_COUNT, recordTrgo, NULL));

    buildTrgoTimebase(HW_TIM_TRGO_UPDATE);
    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgo, NULL));
    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, NULL, NULL));
    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);

    TEST_ASSERT_TRUE(HW_TIM_registerTrgoCallback(BASE_PERIPH, recordTrgoAlt, NULL));
    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoCount);
    TEST_ASSERT_EQUAL_UINT32(1U, trgoAltCount);

    TEST_ASSERT_TRUE(HW_TIM_init(&timConfig));
    HW_TIM_advanceTime(100U);
    TEST_ASSERT_EQUAL_UINT32(2U, trgoAltCount);
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
    RUN_TEST(test_advanceTime_counts_center_aligned);

    RUN_TEST(test_trgo_update_fires_at_period_boundary);
    RUN_TEST(test_trgo_update_fires_per_crossing_in_one_advance);
    RUN_TEST(test_trgo_update_counts_a_landing_once);
    RUN_TEST(test_trgo_update_on_down_counter);
    RUN_TEST(test_trgo_update_center_fires_at_both_extremes);
    RUN_TEST(test_trgo_update_center_with_rcr_fires_at_the_valley);
    RUN_TEST(test_trgo_update_honours_repetition_counter);
    RUN_TEST(test_trgo_oc_match_fires_at_compare);
    RUN_TEST(test_trgo_oc_match_uses_configured_unit);
    RUN_TEST(test_trgo_oc_match_center_fires_on_both_crossings);
    RUN_TEST(test_trgo_silent_when_not_configured);
    RUN_TEST(test_trgo_registration);

    RUN_TEST(test_ports_registered);
    RUN_TEST(test_unnamed_channel_registers_no_ports);
    RUN_TEST(test_boot_state_published_dark);
    RUN_TEST(test_setCompare_publishes_normalized_duty);
    RUN_TEST(test_setOutputEnabled_publishes_enable);
    RUN_TEST(test_setMainOutputEnabled_publishes_moe);

    return UNITY_END();
}
