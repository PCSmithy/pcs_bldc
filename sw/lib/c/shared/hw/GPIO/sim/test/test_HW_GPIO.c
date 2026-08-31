#include "HW_GPIO.h"
#include "SIL_ports.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

// Behavioral input/EXTI coverage (level injection, edge dispatch) lives in SIL
// (sw/sil/pcs_bldc_sil/tests/gpio_behavior.rs); here Unity covers init, config
// validation, and the seams reachable through the framework hook doubles.

// Single-bit HAL-style pin masks for the pins used by the baseline config.
#define OUT_PIN   (0x0010U)   // line 4  -> output
#define IN_PIN    (0x4000U)   // line 14 -> input
#define IRQ_PIN   (0x2000U)   // line 13 -> interrupt input

// Test-owned SIL_ports hooks double: registerSignal assigns sequential handles
// and remembers each port's local name; writeSignal records the value and write
// count per handle. Installed before HW_GPIO_init so the driver's registrations
// and publications run through the production seam. With no hooks installed the
// driver registers nothing and its writes no-op (the standalone-native contract).
#define MAX_PORTS  (8)
static char     portName[MAX_PORTS][24];
static double   portValue[MAX_PORTS];
static uint32_t portWrites[MAX_PORTS];
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
        portValue[handle] = value;
        portWrites[handle]++;
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

// Drop the boot-state publication so a test counts only its own writes.
static void clearPortWrites(void)
{
    for (int32_t i = 0; i < MAX_PORTS; i++)
    {
        portWrites[i] = 0U;
    }
}

// File-scope config the tests build (good baseline) and tweak per case.
// HW_GPIO_init stores a pointer to it, so it must outlive each test — hence
// file scope rather than a stack local.
static HW_GPIO_pinConfig_S portCPins[3];
static HW_GPIO_config_S    gpioConfig;

// Good baseline: one output, one input, one interrupt pin on port C; all
// other ports left with no pins declared.
static void buildGoodConfig(void)
{
    portCPins[0] = (HW_GPIO_pinConfig_S){
        .pin = OUT_PIN, .mode = HW_GPIO_MODE_OUTPUT,    .pinNameStr = "out" };
    portCPins[1] = (HW_GPIO_pinConfig_S){
        .pin = IN_PIN,  .mode = HW_GPIO_MODE_INPUT,     .pinNameStr = "in" };
    portCPins[2] = (HW_GPIO_pinConfig_S){
        .pin = IRQ_PIN, .mode = HW_GPIO_MODE_INTERRUPT, .pinNameStr = "irq" };

    gpioConfig = (HW_GPIO_config_S){ 0 };
    gpioConfig.ports[HW_GPIO_PORT_C].pins    = portCPins;
    gpioConfig.ports[HW_GPIO_PORT_C].numPins = sizeof(portCPins) / sizeof(portCPins[0]);
}

static void testExtiCallback(HW_GPIO_port_E port, uint32_t pin, void * context)
{
    (void)port; (void)pin; (void)context;
}

void setUp(void)
{
    SIL_ports_setHooks(NULL); // unlinked by default; port tests opt in
    for (int32_t i = 0; i < MAX_PORTS; i++)
    {
        portName[i][0] = '\0';
        portValue[i]   = 0.0;
        portWrites[i]  = 0U;
    }
    portCount = 0;

    // A rejected init is the clean slate: init drops the driver to its
    // uninitialized state before it looks at the config.
    (void)HW_GPIO_init(NULL);
    buildGoodConfig();
}

void tearDown(void)
{
    SIL_ports_setHooks(NULL);
}

/* ---- fw~hal_gpio_001: init + config validation ---- */
// [test->fw~hal_gpio_001~1]
static void test_init_valid_config(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
}

// [test->fw~hal_gpio_001~1]
static void test_init_null_config(void)
{
    TEST_ASSERT_FALSE(HW_GPIO_init(NULL));
}

// [test->fw~hal_gpio_001~1]
static void test_init_empty_pin_mask(void)
{
    portCPins[0].pin = 0x0U; // selects no GPIO line
    TEST_ASSERT_FALSE(HW_GPIO_init(&gpioConfig));
}

// [test->fw~hal_gpio_001~1]
static void test_init_out_of_range_mode(void)
{
    portCPins[0].mode = (HW_GPIO_mode_E)99;
    TEST_ASSERT_FALSE(HW_GPIO_init(&gpioConfig));
}

// [test->fw~hal_gpio_001~1]
static void test_reinit_is_a_clean_slate(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    TEST_ASSERT_EQUAL_INT32(1, portCount);

    // A second init registers afresh and publishes the boot state on the new
    // handle; writes land there, not on the first registration.
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    TEST_ASSERT_EQUAL_INT32(2, portCount);
    clearPortWrites();
    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, portWrites[1]);

    // A rejected init drops the driver back to uninitialized: writes no-op.
    TEST_ASSERT_FALSE(HW_GPIO_init(NULL));
    clearPortWrites();
    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[1]);
}

/* ---- fw~hal_gpio_002: pin configuration validity (via init) ---- */
// [test->fw~hal_gpio_002~1]
static void test_pin_validity_undefined_line(void)
{
    portCPins[0].pin = 0x10000U; // bit above line 15
    TEST_ASSERT_FALSE(HW_GPIO_init(&gpioConfig));
}

// [test->fw~hal_gpio_002~1]
static void test_pin_validity_good_config(void)
{
    // Defined lines + supported modes -> valid.
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
}

/* ---- fw~hal_gpio_003: output pin write ---- */
// [test->fw~hal_gpio_003~1]
static void test_output_write_high_then_low(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    const int32_t h = portHandle("out");
    TEST_ASSERT_TRUE(h >= 0);
    clearPortWrites();

    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(1U, portWrites[h]);
    TEST_ASSERT_TRUE(portValue[h] == 1.0);

    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_LOW);
    TEST_ASSERT_EQUAL_UINT32(2U, portWrites[h]);
    TEST_ASSERT_TRUE(portValue[h] == 0.0);
}

// [test->fw~hal_gpio_003~1]
static void test_output_write_out_of_range_port_is_noop(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    const int32_t h = portHandle("out");
    TEST_ASSERT_TRUE(h >= 0);
    clearPortWrites();

    // Writing to an out-of-range port does nothing and does not crash.
    HW_GPIO_writePin(HW_GPIO_PORT_COUNT, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[h]);
}

// [test->fw~hal_gpio_003~1]
static void test_output_write_before_init_is_noop(void)
{
    installPortsDouble();
    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_INT32(0, portCount);
}

/* ---- fw~hal_gpio_004: cached input snapshot ---- */
// [test->fw~hal_gpio_004~1]
static void test_cached_read_before_first_pass_low(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    // The cache holds nothing until a sampling pass -> low.
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readCached(HW_GPIO_PORT_C, IN_PIN));
}

// [test->fw~hal_gpio_004~1]
static void test_cached_read_non_input_pin_low(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    HW_GPIO_run1ms();
    // OUT_PIN is an output, not a configured input -> never captured.
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readCached(HW_GPIO_PORT_C, OUT_PIN));
}

// [test->fw~hal_gpio_004~1]
static void test_cached_read_out_of_range_port_low(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    HW_GPIO_run1ms();
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readCached(HW_GPIO_PORT_COUNT, IN_PIN));
}

/* ---- fw~hal_gpio_005: pin-change interrupt callbacks ---- */
// [test->fw~hal_gpio_005~1]
static void test_exti_register_valid_port(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    int ctx = 0;
    TEST_ASSERT_TRUE(HW_GPIO_registerExtiCallback(HW_GPIO_PORT_C, IRQ_PIN, testExtiCallback, &ctx));
}

// [test->fw~hal_gpio_005~1]
static void test_exti_register_out_of_range_port(void)
{
    int ctx = 0;
    TEST_ASSERT_FALSE(HW_GPIO_registerExtiCallback(HW_GPIO_PORT_COUNT, IRQ_PIN, testExtiCallback, &ctx));
}

/* ---- output-pin observation ports ---- */

// Only the named output pin registers a port; inputs and interrupt inputs don't.
static void test_ports_registered(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    TEST_ASSERT_EQUAL_INT32(1, portCount);
    TEST_ASSERT_TRUE(portHandle("out") >= 0);
    TEST_ASSERT_TRUE(portHandle("in") < 0);
    TEST_ASSERT_TRUE(portHandle("irq") < 0);
}

// An unnamed output pin registers nothing, and a write to it publishes nothing.
static void test_unnamed_pin_registers_no_ports(void)
{
    installPortsDouble();
    portCPins[0].pinNameStr = NULL;
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    TEST_ASSERT_EQUAL_INT32(0, portCount);
    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0U, portWrites[0]);
}

// Init publishes the boot state: every output pin reads low until firmware drives it.
static void test_boot_state_published_low(void)
{
    installPortsDouble();
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    const int32_t h = portHandle("out");
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_EQUAL_UINT32(1U, portWrites[h]);
    TEST_ASSERT_TRUE(portValue[h] == 0.0);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_empty_pin_mask);
    RUN_TEST(test_init_out_of_range_mode);
    RUN_TEST(test_reinit_is_a_clean_slate);

    RUN_TEST(test_pin_validity_undefined_line);
    RUN_TEST(test_pin_validity_good_config);

    RUN_TEST(test_output_write_high_then_low);
    RUN_TEST(test_output_write_out_of_range_port_is_noop);
    RUN_TEST(test_output_write_before_init_is_noop);

    RUN_TEST(test_cached_read_before_first_pass_low);
    RUN_TEST(test_cached_read_non_input_pin_low);
    RUN_TEST(test_cached_read_out_of_range_port_low);

    RUN_TEST(test_exti_register_valid_port);
    RUN_TEST(test_exti_register_out_of_range_port);

    RUN_TEST(test_ports_registered);
    RUN_TEST(test_unnamed_pin_registers_no_ports);
    RUN_TEST(test_boot_state_published_low);

    return UNITY_END();
}
