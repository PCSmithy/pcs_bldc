#include "HW_GPIO.h"
#include "HW_GPIO_sim.h"
#include "unity.h"

// Single-bit HAL-style pin masks for the pins used by the baseline config.
#define OUT_PIN   (0x0010U)   // line 4  -> output
#define IN_PIN    (0x4000U)   // line 14 -> input
#define IRQ_PIN   (0x2000U)   // line 13 -> interrupt input

// File-scope config the tests build (good baseline) and tweak per case.
// HW_GPIO_init stores a pointer to it, so it must outlive each test — hence
// file scope rather than a stack local.
static HW_GPIO_pinConfig_S portCPins[3];
static HW_GPIO_config_S    gpioConfig;

// EXTI callback observation.
static uint32_t          cbCount;
static HW_GPIO_port_E    cbPort;
static uint32_t          cbPin;
static void *            cbContext;

static void testExtiCallback(HW_GPIO_port_E port, uint32_t pin, void * context)
{
    cbCount++;
    cbPort    = port;
    cbPin     = pin;
    cbContext = context;
}

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

void setUp(void)
{
    HW_GPIO_sim_reset();
    cbCount   = 0U;
    cbPort    = HW_GPIO_PORT_COUNT;
    cbPin     = 0U;
    cbContext = NULL;
    buildGoodConfig();
}

void tearDown(void) {}

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
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_HIGH, HW_GPIO_sim_getLevel(HW_GPIO_PORT_C, OUT_PIN));
    TEST_ASSERT_EQUAL_UINT32(1U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_C, OUT_PIN));

    HW_GPIO_writePin(HW_GPIO_PORT_C, OUT_PIN, HW_GPIO_LEVEL_LOW);
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_sim_getLevel(HW_GPIO_PORT_C, OUT_PIN));
    TEST_ASSERT_EQUAL_UINT32(2U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_C, OUT_PIN));
}

// [test->fw~hal_gpio_003~1]
static void test_output_write_out_of_range_port_is_noop(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    // Writing to an out-of-range port does nothing and does not crash.
    HW_GPIO_writePin(HW_GPIO_PORT_COUNT, OUT_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_GPIO_sim_getWriteCount(HW_GPIO_PORT_C, OUT_PIN));
}

/* ---- fw~hal_gpio_004: input pin read ---- */
// [test->fw~hal_gpio_004~1]
static void test_input_read_injected_level(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    HW_GPIO_sim_setInputLevel(HW_GPIO_PORT_C, IN_PIN, HW_GPIO_LEVEL_HIGH);
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_HIGH, HW_GPIO_readPin(HW_GPIO_PORT_C, IN_PIN));

    HW_GPIO_sim_setInputLevel(HW_GPIO_PORT_C, IN_PIN, HW_GPIO_LEVEL_LOW);
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readPin(HW_GPIO_PORT_C, IN_PIN));
}

// [test->fw~hal_gpio_004~1]
static void test_input_read_default_low(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    // Nothing injected -> defaults to low.
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readPin(HW_GPIO_PORT_C, IN_PIN));
}

// [test->fw~hal_gpio_004~1]
static void test_input_read_out_of_range_port_low(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));
    TEST_ASSERT_EQUAL_INT(HW_GPIO_LEVEL_LOW, HW_GPIO_readPin(HW_GPIO_PORT_COUNT, IN_PIN));
}

/* ---- fw~hal_gpio_005: pin-change interrupt callbacks ---- */
// [test->fw~hal_gpio_005~1]
static void test_exti_callback_fires_once(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    int ctx = 0;
    TEST_ASSERT_TRUE(HW_GPIO_registerExtiCallback(HW_GPIO_PORT_C, IRQ_PIN, testExtiCallback, &ctx));

    HW_GPIO_sim_triggerExti(HW_GPIO_PORT_C, IRQ_PIN);
    TEST_ASSERT_EQUAL_UINT32(1U, cbCount);
    TEST_ASSERT_EQUAL_INT(HW_GPIO_PORT_C, cbPort);
    TEST_ASSERT_EQUAL_UINT32(IRQ_PIN, cbPin);
    TEST_ASSERT_EQUAL_PTR(&ctx, cbContext);
}

// [test->fw~hal_gpio_005~1]
static void test_exti_no_callback_invokes_nothing(void)
{
    TEST_ASSERT_TRUE(HW_GPIO_init(&gpioConfig));

    // No callback registered on this edge -> nothing fires.
    HW_GPIO_sim_triggerExti(HW_GPIO_PORT_C, IRQ_PIN);
    TEST_ASSERT_EQUAL_UINT32(0U, cbCount);
}

// [test->fw~hal_gpio_005~1]
static void test_exti_register_out_of_range_port(void)
{
    int ctx = 0;
    TEST_ASSERT_FALSE(HW_GPIO_registerExtiCallback(HW_GPIO_PORT_COUNT, IRQ_PIN, testExtiCallback, &ctx));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_config);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_empty_pin_mask);
    RUN_TEST(test_init_out_of_range_mode);

    RUN_TEST(test_pin_validity_undefined_line);
    RUN_TEST(test_pin_validity_good_config);

    RUN_TEST(test_output_write_high_then_low);
    RUN_TEST(test_output_write_out_of_range_port_is_noop);

    RUN_TEST(test_input_read_injected_level);
    RUN_TEST(test_input_read_default_low);
    RUN_TEST(test_input_read_out_of_range_port_low);

    RUN_TEST(test_exti_callback_fires_once);
    RUN_TEST(test_exti_no_callback_invokes_nothing);
    RUN_TEST(test_exti_register_out_of_range_port);

    return UNITY_END();
}
