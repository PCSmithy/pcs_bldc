#include "lib_filterIIR.h"
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

static lib_filterIIR_channel_S makeEma(float32_t alpha)
{
    lib_filterIIR_channel_S filter = { 0 };
    filter.type = LIB_FILTERIIR_TYPE_EMA;
    filter.ema.alpha = alpha;
    return filter;
}

/* ---- init validation ---- */

static void test_init_valid_ema(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));
    TEST_ASSERT_TRUE(filter.init);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, filter.ema.y_k);   // seeded from x_k, 0 here
}

static void test_init_seeds_output_to_input(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    filter.ema.x_k = 25.0f;
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, filter.ema.y_k);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, filter.ema.y_k_1);

    // A seeded filter is already converged: updates holding the seed value
    // stay there — no startup transient.
    for (uint32_t k = 0U; k < 10U; k++)
    {
        lib_filterIIR_update(&filter);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, filter.ema.y_k);
    }

    // And a step from the seed converges per the same closed form, from the
    // seed rather than from zero: y[k] = 20 + 5*(1-a)^k.
    filter.ema.x_k = 20.0f;
    for (uint32_t k = 1U; k <= 10U; k++)
    {
        lib_filterIIR_update(&filter);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f + (5.0f * powf(0.9f, 10.0f)), filter.ema.y_k);
}

static void test_init_null_rejected(void)
{
    TEST_ASSERT_FALSE(lib_filterIIR_init(NULL));
}

static void test_init_rejects_invalid_type(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    filter.type = LIB_FILTERIIR_TYPE_COUNT;
    TEST_ASSERT_FALSE(lib_filterIIR_init(&filter));
    TEST_ASSERT_FALSE(filter.init);
}

static void test_init_rejects_alpha_out_of_range(void)
{
    lib_filterIIR_channel_S zero = makeEma(0.0f);
    TEST_ASSERT_FALSE(lib_filterIIR_init(&zero));

    lib_filterIIR_channel_S negative = makeEma(-0.1f);
    TEST_ASSERT_FALSE(lib_filterIIR_init(&negative));

    lib_filterIIR_channel_S excess = makeEma(1.5f);
    TEST_ASSERT_FALSE(lib_filterIIR_init(&excess));

    lib_filterIIR_channel_S unity_alpha = makeEma(1.0f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&unity_alpha));
}

static void test_reinit_after_bad_type_stays_unrunnable(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.5f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));

    filter.type = LIB_FILTERIIR_TYPE_COUNT;
    TEST_ASSERT_FALSE(lib_filterIIR_init(&filter));

    // A failed re-init leaves the filter inert: update must not run.
    filter.type = LIB_FILTERIIR_TYPE_EMA;
    filter.ema.x_k = 100.0f;
    lib_filterIIR_update(&filter);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, filter.ema.y_k);
}

static void test_update_before_init_is_inert(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.5f);
    filter.ema.x_k = 42.0f;
    lib_filterIIR_update(&filter);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, filter.ema.y_k);

    lib_filterIIR_update(NULL);   // must not crash
}

/* ---- EMA recurrence: y[k] = (1-a)y[k-1] + a*x[k] ---- */

static void test_ema_recurrence_exact_first_steps(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));

    filter.ema.x_k = 10.0f;
    lib_filterIIR_update(&filter);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, filter.ema.y_k);     // 0.9*0 + 0.1*10
    TEST_ASSERT_EQUAL_FLOAT(0.0f, filter.ema.y_k_1);

    lib_filterIIR_update(&filter);
    TEST_ASSERT_EQUAL_FLOAT(1.9f, filter.ema.y_k);     // 0.9*1 + 1
    TEST_ASSERT_EQUAL_FLOAT(1.0f, filter.ema.y_k_1);

    lib_filterIIR_update(&filter);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.71f, filter.ema.y_k);
    TEST_ASSERT_EQUAL_FLOAT(1.9f, filter.ema.y_k_1);
}

static void test_ema_step_response_matches_closed_form(void)
{
    // y[k] on a unit step is 1 - (1-a)^k; check the discrete closed form at
    // k = 10 (one time constant for a = dt/tau = 0.1) and k = 50.
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));
    filter.ema.x_k = 1.0f;

    for (uint32_t k = 1U; k <= 50U; k++)
    {
        lib_filterIIR_update(&filter);
        if (k == 10U)
        {
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f - powf(0.9f, 10.0f), filter.ema.y_k);   // 0.6513
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f - powf(0.9f, 50.0f), filter.ema.y_k);           // 0.9948
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, filter.ema.y_k);   // within 1% after ~5 tau
}

static void test_ema_dc_gain_is_unity(void)
{
    lib_filterIIR_channel_S filter = makeEma(0.25f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));
    filter.ema.x_k = -7.5f;
    for (uint32_t k = 0U; k < 200U; k++)
    {
        lib_filterIIR_update(&filter);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -7.5f, filter.ema.y_k);
}

static void test_ema_alpha_one_is_passthrough(void)
{
    lib_filterIIR_channel_S filter = makeEma(1.0f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));

    const float32_t inputs[4] = { 3.0f, -2.0f, 0.0f, 99.5f };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        filter.ema.x_k = inputs[i];
        lib_filterIIR_update(&filter);
        TEST_ASSERT_EQUAL_FLOAT(inputs[i], filter.ema.y_k);
    }
}

static void test_ema_attenuates_alternating_input(void)
{
    // The Nyquist-rate component (+1/-1 each sample) settles to an output
    // amplitude of a/(2-a) — 0.0526 for a = 0.1. Verify strong rejection.
    lib_filterIIR_channel_S filter = makeEma(0.1f);
    TEST_ASSERT_TRUE(lib_filterIIR_init(&filter));

    float32_t peak = 0.0f;
    for (uint32_t k = 0U; k < 400U; k++)
    {
        filter.ema.x_k = (((k % 2U) == 0U)) ? 1.0f : -1.0f;
        lib_filterIIR_update(&filter);
        if ((k >= 200U) && (fabsf(filter.ema.y_k) > peak))
        {
            peak = fabsf(filter.ema.y_k);
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.1f / 1.9f, peak);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_valid_ema);
    RUN_TEST(test_init_seeds_output_to_input);
    RUN_TEST(test_init_null_rejected);
    RUN_TEST(test_init_rejects_invalid_type);
    RUN_TEST(test_init_rejects_alpha_out_of_range);
    RUN_TEST(test_reinit_after_bad_type_stays_unrunnable);
    RUN_TEST(test_update_before_init_is_inert);

    RUN_TEST(test_ema_recurrence_exact_first_steps);
    RUN_TEST(test_ema_step_response_matches_closed_form);
    RUN_TEST(test_ema_dc_gain_is_unity);
    RUN_TEST(test_ema_alpha_one_is_passthrough);
    RUN_TEST(test_ema_attenuates_alternating_input);

    return UNITY_END();
}
