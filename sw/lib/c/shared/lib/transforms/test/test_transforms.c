#include "lib_transforms.h"
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- double-precision references, formulated independently ---- */

#define PI_D     (3.14159265358979323846)   // <math.h> hides PI_D under -std=c11
#define TWO_PI_3 (2.0 * PI_D / 3.0)

// Clarke as the projection of the three phase axes (0, -120, +120 deg) onto
// the stationary frame, amplitude-invariant.
static void ref_clarke(double a, double b, double c, double * alpha, double * beta)
{
    *alpha = (2.0 / 3.0) * (a + (b * cos(TWO_PI_3)) + (c * cos(-TWO_PI_3)));
    *beta  = (2.0 / 3.0) * ((b * sin(TWO_PI_3)) + (c * sin(-TWO_PI_3)));
}

// Park as the rotation matrix R(-theta); inverse Park its transpose.
static void ref_park(double alpha, double beta, double theta, double * d, double * q)
{
    *d = ( cos(theta) * alpha) + (sin(theta) * beta);
    *q = (-sin(theta) * alpha) + (cos(theta) * beta);
}

static void ref_inverse_park(double d, double q, double theta, double * alpha, double * beta)
{
    *alpha = (cos(theta) * d) - (sin(theta) * q);
    *beta  = (sin(theta) * d) + (cos(theta) * q);
}

// fw~mc_013 tolerance: 1e-5 of the input magnitude.
static float tol(double magnitude)
{
    return (float)(1e-5 * magnitude);
}

static const double AMPLITUDES[] = { 0.1, 1.0, 10.0, 100.0 };
#define N_AMPLITUDES (sizeof(AMPLITUDES) / sizeof(AMPLITUDES[0]))
#define N_ANGLES     (72U)   // 5 degree steps over a full turn

/* ---- fw~mc_013: reference sweeps ---- */

// [test->fw~mc_013~1]
static void test_clarke_matches_reference_over_balanced_sweep(void)
{
    for (size_t i = 0U; i < N_AMPLITUDES; i++)
    {
        const double A = AMPLITUDES[i];
        for (uint32_t k = 0U; k < N_ANGLES; k++)
        {
            const double phi = (2.0 * PI_D * k) / N_ANGLES;
            const double a = A * cos(phi);
            const double b = A * cos(phi - TWO_PI_3);
            const double c = A * cos(phi + TWO_PI_3);

            double expAlpha, expBeta;
            ref_clarke(a, b, c, &expAlpha, &expBeta);

            float32_t alpha = 0.0f, beta = 0.0f;
            lib_transforms_clarke((float32_t)a, (float32_t)b, (float32_t)c, &alpha, &beta);

            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expAlpha, alpha);
            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expBeta, beta);
        }
    }
}

// Unbalanced inputs too: the reference is the general projection, so a set
// with a zero-sequence component must still agree.
// [test->fw~mc_013~1]
static void test_clarke_matches_reference_on_unbalanced_sets(void)
{
    const double sets[][3] = {
        { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 1.0 },
        { 3.0, -1.0, 2.0 },
        { 5.0, 5.0, 5.0 },     // pure zero-sequence maps to the origin
        { -2.5, 0.75, 10.0 },
    };
    for (size_t i = 0U; i < (sizeof(sets) / sizeof(sets[0])); i++)
    {
        const double a = sets[i][0], b = sets[i][1], c = sets[i][2];
        const double mag = sqrt((a * a) + (b * b) + (c * c));

        double expAlpha, expBeta;
        ref_clarke(a, b, c, &expAlpha, &expBeta);

        float32_t alpha = 0.0f, beta = 0.0f;
        lib_transforms_clarke((float32_t)a, (float32_t)b, (float32_t)c, &alpha, &beta);

        TEST_ASSERT_FLOAT_WITHIN(tol(mag), (float)expAlpha, alpha);
        TEST_ASSERT_FLOAT_WITHIN(tol(mag), (float)expBeta, beta);
    }
}

// [test->fw~mc_013~1]
static void test_park_matches_reference_over_sweep(void)
{
    for (size_t i = 0U; i < N_AMPLITUDES; i++)
    {
        const double A = AMPLITUDES[i];
        for (uint32_t k = 0U; k < N_ANGLES; k++)
        {
            const double theta = (2.0 * PI_D * k) / N_ANGLES;
            // A vector at a different angle from the frame, so d and q are both exercised.
            const double alpha = A * cos(theta + 0.7);
            const double beta  = A * sin(theta + 0.7);

            double expD, expQ;
            ref_park(alpha, beta, theta, &expD, &expQ);

            float32_t d = 0.0f, q = 0.0f;
            lib_transforms_park((float32_t)alpha, (float32_t)beta, (float32_t)theta, &d, &q);

            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expD, d);
            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expQ, q);
        }
    }
}

// [test->fw~mc_013~1]
static void test_inverse_park_matches_reference_over_sweep(void)
{
    for (size_t i = 0U; i < N_AMPLITUDES; i++)
    {
        const double A = AMPLITUDES[i];
        for (uint32_t k = 0U; k < N_ANGLES; k++)
        {
            const double theta = (2.0 * PI_D * k) / N_ANGLES;
            const double d = A * 0.6;
            const double q = A * -0.8;

            double expAlpha, expBeta;
            ref_inverse_park(d, q, theta, &expAlpha, &expBeta);

            float32_t alpha = 0.0f, beta = 0.0f;
            lib_transforms_inversePark((float32_t)d, (float32_t)q, (float32_t)theta, &alpha, &beta);

            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expAlpha, alpha);
            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)expBeta, beta);
        }
    }
}

/* ---- fw~mc_013: structural properties ---- */

// [test->fw~mc_013~1]
static void test_park_then_inverse_park_returns_input(void)
{
    for (size_t i = 0U; i < N_AMPLITUDES; i++)
    {
        const double A = AMPLITUDES[i];
        for (uint32_t k = 0U; k < N_ANGLES; k++)
        {
            const float32_t theta = (float32_t)((2.0 * PI_D * k) / N_ANGLES);
            const float32_t alphaIn = (float32_t)(A * 0.3);
            const float32_t betaIn  = (float32_t)(A * -0.95);

            float32_t d = 0.0f, q = 0.0f;
            lib_transforms_park(alphaIn, betaIn, theta, &d, &q);
            float32_t alphaOut = 0.0f, betaOut = 0.0f;
            lib_transforms_inversePark(d, q, theta, &alphaOut, &betaOut);

            TEST_ASSERT_FLOAT_WITHIN(tol(A), alphaIn, alphaOut);
            TEST_ASSERT_FLOAT_WITHIN(tol(A), betaIn, betaOut);
        }
    }
}

// [test->fw~mc_013~1]
static void test_balanced_set_of_amplitude_A_has_stationary_magnitude_A(void)
{
    for (size_t i = 0U; i < N_AMPLITUDES; i++)
    {
        const double A = AMPLITUDES[i];
        for (uint32_t k = 0U; k < N_ANGLES; k++)
        {
            const double phi = (2.0 * PI_D * k) / N_ANGLES;
            const float32_t a = (float32_t)(A * cos(phi));
            const float32_t b = (float32_t)(A * cos(phi - TWO_PI_3));
            const float32_t c = (float32_t)(A * cos(phi + TWO_PI_3));

            float32_t alpha = 0.0f, beta = 0.0f;
            lib_transforms_clarke(a, b, c, &alpha, &beta);

            const float32_t mag = sqrtf((alpha * alpha) + (beta * beta));
            TEST_ASSERT_FLOAT_WITHIN(tol(A), (float)A, mag);
        }
    }
}

/* ---- spot values and the output contract ---- */

static void test_clarke_spot_values(void)
{
    float32_t alpha = 0.0f, beta = 0.0f;

    // Phase A aligned with alpha.
    lib_transforms_clarke(1.0f, -0.5f, -0.5f, &alpha, &beta);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, alpha);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, beta);

    // Phase B leads phase C by the beta axis: b = -c = sqrt(3)/2 gives beta = 1.
    lib_transforms_clarke(0.0f, 0.8660254f, -0.8660254f, &alpha, &beta);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, alpha);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, beta);
}

static void test_park_spot_values(void)
{
    float32_t d = 0.0f, q = 0.0f;

    // Frame at 0: identity.
    lib_transforms_park(0.25f, -0.75f, 0.0f, &d, &q);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, d);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.75f, q);

    // Frame at 90 degrees: (d, q) = (beta, -alpha).
    lib_transforms_park(0.25f, -0.75f, (float32_t)(PI_D / 2.0), &d, &q);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.75f, d);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.25f, q);
}

static void test_null_output_leaves_both_outputs_untouched(void)
{
    float32_t x = 42.0f;

    lib_transforms_clarke(1.0f, 2.0f, 3.0f, &x, NULL);
    lib_transforms_clarke(1.0f, 2.0f, 3.0f, NULL, &x);
    lib_transforms_park(1.0f, 2.0f, 0.5f, &x, NULL);
    lib_transforms_park(1.0f, 2.0f, 0.5f, NULL, &x);
    lib_transforms_inversePark(1.0f, 2.0f, 0.5f, &x, NULL);
    lib_transforms_inversePark(1.0f, 2.0f, 0.5f, NULL, &x);
    lib_transforms_svm(1.0f, 2.0f, 12.0f, &x, &x, NULL);
    lib_transforms_svm(1.0f, 2.0f, 12.0f, &x, NULL, &x);
    lib_transforms_svm(1.0f, 2.0f, 12.0f, NULL, &x, &x);

    TEST_ASSERT_EQUAL_FLOAT(42.0f, x);
}

/* ---- fw~mc_014: space-vector modulation ---- */

// Reference in double: the phase voltages as projections onto the phase axes
// (0, +120, -120 deg), the clamp by magnitude, then the min/max zero-sequence law.
static void ref_svm(double vAlpha, double vBeta, double vBus, double d[3])
{
    if (vBus <= 0.0)
    {
        d[0] = d[1] = d[2] = 0.5;
        return;
    }
    const double limit = vBus / sqrt(3.0);
    const double mag = sqrt((vAlpha * vAlpha) + (vBeta * vBeta));
    if (mag > limit)
    {
        vAlpha *= limit / mag;
        vBeta  *= limit / mag;
    }
    const double axis[3] = { 0.0, TWO_PI_3, -TWO_PI_3 };
    double v[3];
    for (int i = 0; i < 3; i++)
    {
        v[i] = (vAlpha * cos(axis[i])) + (vBeta * sin(axis[i]));
    }
    const double vMax = fmax(v[0], fmax(v[1], v[2]));
    const double vMin = fmin(v[0], fmin(v[1], v[2]));
    for (int i = 0; i < 3; i++)
    {
        d[i] = 0.5 + ((v[i] - ((vMax + vMin) / 2.0)) / vBus);
    }
}

static const double BUS_VOLTAGES[] = { 5.0, 12.0, 20.0, 24.0 };
#define N_BUS (sizeof(BUS_VOLTAGES) / sizeof(BUS_VOLTAGES[0]))

// Command magnitudes as a fraction of the linear limit V_bus / sqrt(3).
static const double MAG_FRACTIONS[] = { 0.0, 0.25, 0.5, 0.9, 1.0, 1.3, 2.0 };
#define N_MAG (sizeof(MAG_FRACTIONS) / sizeof(MAG_FRACTIONS[0]))

// [test->fw~mc_014~1]
static void test_svm_matches_reference_including_overmodulation(void)
{
    for (size_t b = 0U; b < N_BUS; b++)
    {
        const double vBus = BUS_VOLTAGES[b];
        const double limit = vBus / sqrt(3.0);
        for (size_t m = 0U; m < N_MAG; m++)
        {
            for (uint32_t k = 0U; k < N_ANGLES; k++)
            {
                const double ang = (2.0 * PI_D * k) / N_ANGLES;
                const double vAlpha = MAG_FRACTIONS[m] * limit * cos(ang);
                const double vBeta  = MAG_FRACTIONS[m] * limit * sin(ang);

                double exp[3];
                ref_svm(vAlpha, vBeta, vBus, exp);

                float32_t dU = -1.0f, dV = -1.0f, dW = -1.0f;
                lib_transforms_svm((float32_t)vAlpha, (float32_t)vBeta, (float32_t)vBus, &dU, &dV, &dW);

                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)exp[0], dU);
                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)exp[1], dV);
                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)exp[2], dW);
            }
        }
    }
}

// [test->fw~mc_014~1]
static void test_svm_duties_lie_in_unit_interval_for_any_command(void)
{
    for (size_t b = 0U; b < N_BUS; b++)
    {
        const float32_t vBus = (float32_t)BUS_VOLTAGES[b];
        for (size_t m = 0U; m < N_MAG; m++)
        {
            for (uint32_t k = 0U; k < N_ANGLES; k++)
            {
                const double ang = (2.0 * PI_D * k) / N_ANGLES;
                const float32_t mag = (float32_t)(MAG_FRACTIONS[m] * vBus);   // up to 2 x V_bus, well past the limit
                float32_t d[3] = { -1.0f, -1.0f, -1.0f };
                lib_transforms_svm(mag * (float32_t)cos(ang), mag * (float32_t)sin(ang), vBus, &d[0], &d[1], &d[2]);
                for (int i = 0; i < 3; i++)
                {
                    TEST_ASSERT_TRUE(d[i] >= -1e-6f);
                    TEST_ASSERT_TRUE(d[i] <= 1.0f + 1e-6f);
                }
            }
        }
    }
}

// [test->fw~mc_014~1]
static void test_svm_duty_differences_equal_voltage_differences_within_limit(void)
{
    const double fractions[] = { 0.1, 0.5, 0.99 };
    for (size_t b = 0U; b < N_BUS; b++)
    {
        const double vBus = BUS_VOLTAGES[b];
        const double limit = vBus / sqrt(3.0);
        for (size_t m = 0U; m < (sizeof(fractions) / sizeof(fractions[0])); m++)
        {
            for (uint32_t k = 0U; k < N_ANGLES; k++)
            {
                const double ang = (2.0 * PI_D * k) / N_ANGLES;
                const double vAlpha = fractions[m] * limit * cos(ang);
                const double vBeta  = fractions[m] * limit * sin(ang);
                const double vU = vAlpha;
                const double vV = (-vAlpha + (sqrt(3.0) * vBeta)) / 2.0;
                const double vW = (-vAlpha - (sqrt(3.0) * vBeta)) / 2.0;

                float32_t dU, dV, dW;
                lib_transforms_svm((float32_t)vAlpha, (float32_t)vBeta, (float32_t)vBus, &dU, &dV, &dW);

                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)((vU - vV) / vBus), dU - dV);
                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)((vV - vW) / vBus), dV - dW);
                TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)((vW - vU) / vBus), dW - dU);
            }
        }
    }
}

// [test->fw~mc_014~1]
static void test_svm_duties_reach_zero_and_one_over_a_cycle_at_the_limit(void)
{
    const float32_t vBus = 24.0f;
    const double limit = vBus / sqrt(3.0);
    float32_t lo[3] = { 2.0f, 2.0f, 2.0f };
    float32_t hi[3] = { -1.0f, -1.0f, -1.0f };

    for (uint32_t k = 0U; k < 360U; k++)
    {
        const double ang = (2.0 * PI_D * k) / 360.0;
        float32_t d[3];
        lib_transforms_svm((float32_t)(limit * cos(ang)), (float32_t)(limit * sin(ang)), vBus, &d[0], &d[1], &d[2]);
        for (int i = 0; i < 3; i++)
        {
            lo[i] = (d[i] < lo[i]) ? d[i] : lo[i];
            hi[i] = (d[i] > hi[i]) ? d[i] : hi[i];
        }
    }
    for (int i = 0; i < 3; i++)
    {
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, lo[i]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, hi[i]);
    }
}

// [test->fw~mc_014~1]
static void test_svm_overmodulated_command_keeps_its_angle(void)
{
    const float32_t vBus = 12.0f;
    const double limit = vBus / sqrt(3.0);
    for (uint32_t k = 0U; k < N_ANGLES; k++)
    {
        const double ang = (2.0 * PI_D * k) / N_ANGLES;
        float32_t atLimit[3], beyond[3];
        lib_transforms_svm((float32_t)(limit * cos(ang)), (float32_t)(limit * sin(ang)), vBus,
                           &atLimit[0], &atLimit[1], &atLimit[2]);
        lib_transforms_svm((float32_t)(3.0 * limit * cos(ang)), (float32_t)(3.0 * limit * sin(ang)), vBus,
                           &beyond[0], &beyond[1], &beyond[2]);
        for (int i = 0; i < 3; i++)
        {
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, atLimit[i], beyond[i]);
        }
    }
}

// [test->fw~mc_014~1]
static void test_svm_bus_voltage_at_or_below_zero_gives_half_duties(void)
{
    const float32_t buses[] = { 0.0f, -0.001f, -24.0f };
    for (size_t b = 0U; b < (sizeof(buses) / sizeof(buses[0])); b++)
    {
        float32_t d[3] = { -1.0f, -1.0f, -1.0f };
        lib_transforms_svm(7.0f, -3.0f, buses[b], &d[0], &d[1], &d[2]);
        TEST_ASSERT_EQUAL_FLOAT(0.5f, d[0]);
        TEST_ASSERT_EQUAL_FLOAT(0.5f, d[1]);
        TEST_ASSERT_EQUAL_FLOAT(0.5f, d[2]);
    }
}

static void test_svm_spot_values(void)
{
    float32_t dU, dV, dW;

    // Zero command: every duty at mid-rail.
    lib_transforms_svm(0.0f, 0.0f, 24.0f, &dU, &dV, &dW);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, dU);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, dV);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, dW);

    // Command along alpha at half the limit on a 24 V bus: v = (6.93, -3.46, -3.46),
    // zero-sequence offset (6.93 - 3.46)/2 = 1.73; d = 0.5 + (v - 1.73)/24.
    lib_transforms_svm(6.9282f, 0.0f, 24.0f, &dU, &dV, &dW);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.7165f, dU);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.2835f, dV);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.2835f, dW);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clarke_matches_reference_over_balanced_sweep);
    RUN_TEST(test_clarke_matches_reference_on_unbalanced_sets);
    RUN_TEST(test_park_matches_reference_over_sweep);
    RUN_TEST(test_inverse_park_matches_reference_over_sweep);
    RUN_TEST(test_park_then_inverse_park_returns_input);
    RUN_TEST(test_balanced_set_of_amplitude_A_has_stationary_magnitude_A);
    RUN_TEST(test_clarke_spot_values);
    RUN_TEST(test_park_spot_values);
    RUN_TEST(test_null_output_leaves_both_outputs_untouched);
    RUN_TEST(test_svm_matches_reference_including_overmodulation);
    RUN_TEST(test_svm_duties_lie_in_unit_interval_for_any_command);
    RUN_TEST(test_svm_duty_differences_equal_voltage_differences_within_limit);
    RUN_TEST(test_svm_duties_reach_zero_and_one_over_a_cycle_at_the_limit);
    RUN_TEST(test_svm_overmodulated_command_keeps_its_angle);
    RUN_TEST(test_svm_bus_voltage_at_or_below_zero_gives_half_duties);
    RUN_TEST(test_svm_spot_values);
    return UNITY_END();
}
