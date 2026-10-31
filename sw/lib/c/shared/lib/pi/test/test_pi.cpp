// lib_pi: the discrete PI regulator in fw~mc_023's form and fw~mc_024's
// back-calculation. Numbers are round so the expected sequences are exact:
// kp = 2, ki = 100, T = 50 us, e = 0.5 gives a first step of 1.0 and
// ki * T * e = 0.0025 more on each later step.
#include "lib_pi.hpp"
#include "unity.h"

#define KP        (2.0f)
#define KI        (100.0f)
#define PERIOD_S  (50.0e-6f)
#define ERR       (0.5f)
#define FIRST     (KP * ERR)              // 1.0
#define RAMP      (KI * PERIOD_S * ERR)   // 0.0025
#define V_LIM     (0.8f)

static const PiConfig kNominal = { KP, KI, PERIOD_S };

static PiRegulator pi;

void setUp(void)
{
    pi = PiRegulator{};
    TEST_ASSERT_TRUE(pi.init(kNominal));
}

void tearDown(void) {}

/* ---- init ---- */

static void test_init_rejects_bad_config(void)
{
    PiRegulator r = {};
    TEST_ASSERT_FALSE(r.init({ KP, 0.0f, PERIOD_S }));
    TEST_ASSERT_FALSE(r.init({ KP, KI, 0.0f }));
    TEST_ASSERT_FALSE(r.init({ KP, KI, -PERIOD_S }));
    TEST_ASSERT_TRUE(r.init({ 0.0f, KI, PERIOD_S }));   // a pure integrator is allowed
}

static void test_step_before_init_is_zero(void)
{
    PiRegulator r = {};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.step(ERR));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.step(ERR));
}

static void test_init_zeroes_the_integral(void)
{
    (void)pi.step(ERR);
    (void)pi.step(ERR);
    TEST_ASSERT_TRUE(pi.init(kNominal));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FIRST, pi.step(ERR));
}

/* ---- [test->fw~mc_023~1] the law ---- */

// The first step is kp * e alone; each of the next 20 adds ki * T * e.
static void test_first_step_is_proportional_then_the_integral_ramps(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FIRST, pi.step(ERR));
    for (uint32_t n = 1U; n <= 20U; n++)
    {
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, FIRST + ((float32_t)n * RAMP), pi.step(ERR));
    }
}

static void test_negative_error_mirrors(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -FIRST, pi.step(-ERR));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -(FIRST + RAMP), pi.step(-ERR));
}

static void test_zero_error_holds_the_integral_term(void)
{
    (void)pi.step(ERR);
    (void)pi.step(ERR);   // S = 2 * T * e after this
    const float32_t held = KI * 2.0f * PERIOD_S * ERR;
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, held, pi.step(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, held, pi.step(0.0f));
}

static void test_reset_zeroes_the_integral(void)
{
    for (uint32_t n = 0U; n < 10U; n++)
    {
        (void)pi.step(ERR);
    }
    pi.reset();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FIRST, pi.step(ERR));
}

/* ---- [test->fw~mc_024~1] back-calculation ---- */

// After the caller limits v, the next step at the same error returns the
// limit exactly, however far the integral had run.
static void test_back_calculation_makes_the_next_step_the_limit(void)
{
    for (uint32_t n = 0U; n < 50U; n++)
    {
        (void)pi.step(ERR);
    }
    pi.backCalculate(ERR, V_LIM);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, V_LIM, pi.step(ERR));
}

// Held at the limit by back-calculation every step, then the error drops to
// zero: the step commands V_lim - kp * e, the integral term alone.
static void test_release_from_the_limit_leaves_the_integral_term(void)
{
    for (uint32_t n = 0U; n < 20U; n++)
    {
        (void)pi.step(ERR);
        pi.backCalculate(ERR, V_LIM);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, V_LIM - FIRST, pi.step(0.0f));
}

static void test_back_calculation_before_init_does_nothing(void)
{
    PiRegulator r = {};
    r.backCalculate(ERR, V_LIM);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.step(ERR));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_step_before_init_is_zero);
    RUN_TEST(test_init_zeroes_the_integral);
    RUN_TEST(test_first_step_is_proportional_then_the_integral_ramps);
    RUN_TEST(test_negative_error_mirrors);
    RUN_TEST(test_zero_error_holds_the_integral_term);
    RUN_TEST(test_reset_zeroes_the_integral);
    RUN_TEST(test_back_calculation_makes_the_next_step_the_limit);
    RUN_TEST(test_release_from_the_limit_leaves_the_integral_term);
    RUN_TEST(test_back_calculation_before_init_does_nothing);
    return UNITY_END();
}
