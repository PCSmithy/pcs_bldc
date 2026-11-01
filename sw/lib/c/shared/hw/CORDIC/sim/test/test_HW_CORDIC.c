#include "HW_CORDIC.h"
#include "unity.h"

// The API contract on the sim side. The coprocessor's own accuracy is a
// bench measurement against the same reference values (fw~hal_cordic_002).

#define TOLERANCE   (1.0e-5f)
#define PI_F        (3.14159265358979f)

static const HW_CORDIC_config_S config = { 0 };

void setUp(void) {}
void tearDown(void) {}

/* ---- fw~hal_cordic_001: init contract ---- */

// [test->fw~hal_cordic_002~1] runs first: the driver starts uninitialised.
static void test_before_init_returns_false_and_leaves_outputs(void)
{
    theta_S out = { .sin_rad = 7.0f, .cos_rad = 7.0f };
    TEST_ASSERT_FALSE(HW_CORDIC_sinCos(0.5f, &out));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, out.sin_rad);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, out.cos_rad);
}

// [test->fw~hal_cordic_001~1]
static void test_init_null_config_rejected(void)
{
    TEST_ASSERT_FALSE(HW_CORDIC_init(NULL));
}

// [test->fw~hal_cordic_001~1]
static void test_init_valid_config_accepted(void)
{
    TEST_ASSERT_TRUE(HW_CORDIC_init(&config));
}

/* ---- fw~hal_cordic_002: sine and cosine ---- */

// [test->fw~hal_cordic_002~1]
static void test_null_output_rejected(void)
{
    TEST_ASSERT_TRUE(HW_CORDIC_init(&config));
    TEST_ASSERT_FALSE(HW_CORDIC_sinCos(0.5f, NULL));
}

// [test->fw~hal_cordic_002~1]
static void test_reference_angles(void)
{
    TEST_ASSERT_TRUE(HW_CORDIC_init(&config));
    const float32_t angles[]  = { 0.0f, PI_F / 6.0f, PI_F / 2.0f, PI_F, -PI_F / 6.0f,
                                  (2.0f * PI_F) + (PI_F / 6.0f), -(4.0f * PI_F) + (PI_F / 2.0f) };
    const float32_t sines[]   = { 0.0f, 0.5f, 1.0f, 0.0f, -0.5f, 0.5f, 1.0f };
    const float32_t cosines[] = { 1.0f, 0.86602540f, 0.0f, -1.0f, 0.86602540f, 0.86602540f, 0.0f };
    for (size_t i = 0U; i < (sizeof(angles) / sizeof(angles[0])); i++)
    {
        theta_S out = { .sin_rad = 0.0f, .cos_rad = 0.0f };
        TEST_ASSERT_TRUE(HW_CORDIC_sinCos(angles[i], &out));
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, sines[i], out.sin_rad);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, cosines[i], out.cos_rad);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_before_init_returns_false_and_leaves_outputs);
    RUN_TEST(test_init_null_config_rejected);
    RUN_TEST(test_init_valid_config_accepted);
    RUN_TEST(test_null_output_rejected);
    RUN_TEST(test_reference_angles);
    return UNITY_END();
}
