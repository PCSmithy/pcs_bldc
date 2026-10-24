// lib_matrix: fixed-size matrices with compile-time dimensions. A product
// of mismatched shapes is a compile error, so only the arithmetic needs a
// runtime check here; the static_asserts at the bottom pin the constexpr
// evaluation.
#include "lib_matrix.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

using Mat22 = Mat<float32_t, 2, 2>;
using Mat21 = Mat<float32_t, 2, 1>;
using Mat23 = Mat<float32_t, 2, 3>;
using Mat32 = Mat<float32_t, 3, 2>;
using Mat33 = Mat<float32_t, 3, 3>;

static const Mat23 kA{{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}};
static const Mat32 kB{{{7.0f, 8.0f}, {9.0f, 10.0f}, {11.0f, 12.0f}}};

static void test_brace_init_zeroes_every_element(void)
{
    const Mat23 z{};
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(0.0f, z.e[i][j]);
        }
    }
}

static void test_aggregate_init_is_row_major(void)
{
    TEST_ASSERT_EQUAL_FLOAT(1.0f, kA.e[0][0]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, kA.e[0][2]);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, kA.e[1][0]);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, kA.e[1][2]);
}

static void test_add_is_elementwise(void)
{
    const Mat23 b{{{10.0f, 20.0f, 30.0f}, {40.0f, 50.0f, 60.0f}}};
    const Mat23 sum = kA + b;
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(kA.e[i][j] + b.e[i][j], sum.e[i][j]);
        }
    }
}

static void test_subtract_is_elementwise(void)
{
    const Mat23 b{{{10.0f, 20.0f, 30.0f}, {40.0f, 50.0f, 60.0f}}};
    const Mat23 diff = b - kA;
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(b.e[i][j] - kA.e[i][j], diff.e[i][j]);
        }
    }
}

// (2x3)(3x2) = 2x2: the inner dimension is the one summed over.
static void test_multiply_2x3_by_3x2(void)
{
    const Mat22 p = kA * kB;
    TEST_ASSERT_EQUAL_FLOAT(58.0f, p.e[0][0]);
    TEST_ASSERT_EQUAL_FLOAT(64.0f, p.e[0][1]);
    TEST_ASSERT_EQUAL_FLOAT(139.0f, p.e[1][0]);
    TEST_ASSERT_EQUAL_FLOAT(154.0f, p.e[1][1]);
}

// (3x2)(2x3) = 3x3: the same operands the other way round give a different
// shape, so the loop bounds for rows, columns, and the sum are all exercised.
static void test_multiply_3x2_by_2x3(void)
{
    const Mat33 p = kB * kA;
    const float32_t expected[3][3] = {
        {39.0f, 54.0f, 69.0f},
        {49.0f, 68.0f, 87.0f},
        {59.0f, 82.0f, 105.0f},
    };
    for (size_t i = 0U; i < 3U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(expected[i][j], p.e[i][j]);
        }
    }
}

static void test_identity_leaves_a_vector_unchanged(void)
{
    const Mat22 identity{{{1.0f, 0.0f}, {0.0f, 1.0f}}};
    const Mat21 x{{{0.75f}, {-2.5f}}};
    const Mat21 y = identity * x;
    TEST_ASSERT_EQUAL_FLOAT(0.75f, y.e[0][0]);
    TEST_ASSERT_EQUAL_FLOAT(-2.5f, y.e[1][0]);
}

static void test_scalar_multiply_scales_every_element(void)
{
    const Mat23 s = kA * 0.5f;
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(0.5f * kA.e[i][j], s.e[i][j]);
        }
    }
}

static void test_transpose_swaps_rows_and_columns(void)
{
    const Mat32 t = kA.transpose();
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(kA.e[i][j], t.e[j][i]);
        }
    }
}

static void test_transpose_twice_restores_the_matrix(void)
{
    const Mat23 back = kA.transpose().transpose();
    for (size_t i = 0U; i < 2U; i++)
    {
        for (size_t j = 0U; j < 3U; j++)
        {
            TEST_ASSERT_EQUAL_FLOAT(kA.e[i][j], back.e[i][j]);
        }
    }
}

// The rotor estimator's prediction: x <- F x with F = [[1, T], [0, 1]]
// advances the angle by velocity * T and carries the velocity over.
static void test_constant_velocity_prediction_step(void)
{
    const float32_t T = 50.0e-6f;
    const Mat22 F{{{1.0f, T}, {0.0f, 1.0f}}};
    const Mat21 x{{{1.0f}, {21.0f}}};
    const Mat21 next = F * x;
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 1.0f + (21.0f * T), next.e[0][0]);
    TEST_ASSERT_EQUAL_FLOAT(21.0f, next.e[1][0]);
}

// The rotor estimator's correction: x <- x + K e with a column gain and a
// scalar error.
static void test_correction_step_adds_gain_times_error(void)
{
    const Mat21 K{{{0.6283f}, {98.696f}}};
    const Mat21 x{{{1.0f}, {21.0f}}};
    const float32_t e = 0.01f;
    const Mat21 next = x + (K * e);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f + (0.6283f * e), next.e[0][0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 21.0f + (98.696f * e), next.e[1][0]);
}

// Row vector times column vector collapses to 1x1: H x for H = [1 0].
static void test_row_times_column_is_1x1(void)
{
    const Mat<float32_t, 1, 2> H{{{1.0f, 0.0f}}};
    const Mat21 x{{{0.4f}, {21.0f}}};
    const Mat<float32_t, 1, 1> hx = H * x;
    TEST_ASSERT_EQUAL_FLOAT(0.4f, hx.e[0][0]);
}

// Every operator is usable in a constant expression.
constexpr Mat22 kC{{{1.0f, 2.0f}, {3.0f, 4.0f}}};
constexpr Mat22 kSum = kC + kC;
constexpr Mat22 kDiff = kC - kC;
constexpr Mat22 kProd = kC * kC;
constexpr Mat22 kScaled = kC * 2.0f;
constexpr Mat22 kT = kC.transpose();
static_assert(kSum.e[1][1] == 8.0f, "operator+ is constexpr");
static_assert(kDiff.e[0][1] == 0.0f, "operator- is constexpr");
static_assert(kProd.e[1][0] == 15.0f, "operator* (matrix) is constexpr");
static_assert(kScaled.e[0][1] == 4.0f, "operator* (scalar) is constexpr");
static_assert(kT.e[0][1] == 3.0f, "transpose is constexpr");

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_brace_init_zeroes_every_element);
    RUN_TEST(test_aggregate_init_is_row_major);
    RUN_TEST(test_add_is_elementwise);
    RUN_TEST(test_subtract_is_elementwise);
    RUN_TEST(test_multiply_2x3_by_3x2);
    RUN_TEST(test_multiply_3x2_by_2x3);
    RUN_TEST(test_identity_leaves_a_vector_unchanged);
    RUN_TEST(test_scalar_multiply_scales_every_element);
    RUN_TEST(test_transpose_swaps_rows_and_columns);
    RUN_TEST(test_transpose_twice_restores_the_matrix);
    RUN_TEST(test_constant_velocity_prediction_step);
    RUN_TEST(test_correction_step_adds_gain_times_error);
    RUN_TEST(test_row_times_column_is_1x1);
    return UNITY_END();
}
