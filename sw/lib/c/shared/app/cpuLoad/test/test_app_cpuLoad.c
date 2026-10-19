#include "app_cpuLoad.h"
#include "lib_timer.h"
#include "unity.h"

static uint32_t fakeCounter_us;
static uint32_t test_getTime_us(void) { return fakeCounter_us; }
static void test_enterCritical(void) {}
static void test_exitCritical(void) {}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us    = test_getTime_us,
    .enterCritical = test_enterCritical,
    .exitCritical  = test_exitCritical,
};

// The accumulators only ever grow, so each test measures from its own start.
static uint32_t base[APP_CPULOAD_SLOT_COUNT];

static uint32_t slot(uint32_t i)
{
    return app_cpuLoad_getTime_us(i) - base[i];
}

void setUp(void)
{
    fakeCounter_us = 1000U;
    app_cpuLoad_switchedIn(APP_CPULOAD_SLOT_OTHER);   // a known running slot at a known time
    for (uint32_t i = 0U; i < APP_CPULOAD_SLOT_COUNT; i++)
    {
        base[i] = app_cpuLoad_getTime_us(i);
    }
}

void tearDown(void) {}

// [test->fw~obs_cpu_001~1]
static void test_run_time_lands_on_the_slot_that_was_running(void)
{
    fakeCounter_us = 1000U;
    app_cpuLoad_switchedIn(2U);
    fakeCounter_us = 1300U;
    app_cpuLoad_switchedIn(1U);
    fakeCounter_us = 1350U;
    app_cpuLoad_switchedIn(2U);
    fakeCounter_us = 2000U;
    app_cpuLoad_switchedIn(1U);
    TEST_ASSERT_EQUAL_UINT32(300U + 650U, slot(2U));
    TEST_ASSERT_EQUAL_UINT32(50U, slot(1U));
}

// [test->fw~obs_cpu_001~1]
static void test_slots_partition_the_elapsed_time(void)
{
    const uint32_t start = fakeCounter_us;
    const uint32_t sequence[] = { 1U, 2U, 5U, 1U, 6U, 3U, 1U, 4U, 1U };
    for (uint32_t i = 0U; i < (sizeof(sequence) / sizeof(sequence[0])); i++)
    {
        fakeCounter_us += 37U * (i + 1U);
        app_cpuLoad_switchedIn(sequence[i]);
    }
    uint32_t sum = 0U;
    for (uint32_t i = 0U; i < APP_CPULOAD_SLOT_COUNT; i++)
    {
        sum += slot(i);
    }
    TEST_ASSERT_EQUAL_UINT32(fakeCounter_us - start, sum);
}

// [test->fw~obs_cpu_001~1]
static void test_counter_wrap_charges_the_right_span(void)
{
    fakeCounter_us = 0xFFFFFF00U;
    app_cpuLoad_switchedIn(3U);
    fakeCounter_us = 0x00000100U;   // 512 us later, past the wrap
    app_cpuLoad_switchedIn(1U);
    TEST_ASSERT_EQUAL_UINT32(512U, slot(3U));
}

// [test->fw~obs_cpu_001~1]
static void test_a_number_past_the_slots_lands_in_other(void)
{
    fakeCounter_us = 1000U;
    app_cpuLoad_switchedIn(APP_CPULOAD_SLOT_COUNT + 5U);
    fakeCounter_us = 1010U;
    app_cpuLoad_switchedIn(1U);
    TEST_ASSERT_EQUAL_UINT32(10U, slot(APP_CPULOAD_SLOT_OTHER));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_run_time_lands_on_the_slot_that_was_running);
    RUN_TEST(test_slots_partition_the_elapsed_time);
    RUN_TEST(test_counter_wrap_charges_the_right_span);
    RUN_TEST(test_a_number_past_the_slots_lands_in_other);
    return UNITY_END();
}
