#include "lib_types.h"
#include "lib_utils.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_set_bit_sets_only_that_bit(void)
{
    uint32_t v = 0U;
    SET_BIT_U32(&v, 0U);
    TEST_ASSERT_EQUAL_HEX32(0x00000001U, v);
    SET_BIT_U32(&v, 31U);
    TEST_ASSERT_EQUAL_HEX32(0x80000001U, v);
    SET_BIT_U32(&v, 31U);   // idempotent
    TEST_ASSERT_EQUAL_HEX32(0x80000001U, v);
}

static void test_set_bit_takes_a_pointer_expression(void)
{
    uint32_t v[2] = { 0U, 0U };
    SET_BIT_U32(v + 1, 3U);
    TEST_ASSERT_EQUAL_HEX32(0U, v[0]);
    TEST_ASSERT_EQUAL_HEX32(0x8U, v[1]);
}

static void test_get_bit_reads_each_bit_independently(void)
{
    const uint32_t v = 0x80000002U;
    TEST_ASSERT_FALSE(GET_BIT_U32(v, 0U));
    TEST_ASSERT_TRUE(GET_BIT_U32(v, 1U));
    TEST_ASSERT_FALSE(GET_BIT_U32(v, 2U));
    TEST_ASSERT_TRUE(GET_BIT_U32(v, 31U));
    TEST_ASSERT_FALSE(GET_BIT_U32(0U, 0U));
}

static void test_get_bit_is_usable_as_a_condition(void)
{
    uint32_t due = 0U;
    SET_BIT_U32(&due, 2U);
    uint32_t hits = 0U;
    for (uint32_t bit = 0U; bit < 4U; bit++)
    {
        if (GET_BIT_U32(due, bit))
        {
            hits++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1U, hits);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_bit_sets_only_that_bit);
    RUN_TEST(test_set_bit_takes_a_pointer_expression);
    RUN_TEST(test_get_bit_reads_each_bit_independently);
    RUN_TEST(test_get_bit_is_usable_as_a_condition);
    return UNITY_END();
}
