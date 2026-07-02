#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "unity.h"

void setUp(void)
{
    HW_USB_sim_reset();
    (void)HW_USB_init();
}

void tearDown(void) {}

// [test->fw~hal_usb_001~1]
static void test_init_returns_true(void)
{
    HW_USB_sim_reset();
    TEST_ASSERT_TRUE(HW_USB_init());
}

// [test->fw~hal_usb_002~1]
static void test_connection_state(void)
{
    TEST_ASSERT_FALSE(HW_USB_connected());
    HW_USB_sim_setConnected(true);
    TEST_ASSERT_TRUE(HW_USB_connected());
    HW_USB_sim_setConnected(false);
    TEST_ASSERT_FALSE(HW_USB_connected());
}

// [test->fw~hal_usb_003~1]
static void test_write_accepts_and_reports_count(void)
{
    const uint8_t msg[4] = { 1U, 2U, 3U, 4U };
    TEST_ASSERT_EQUAL_UINT32(4U, HW_USB_write(msg, 4U));

    uint8_t captured[4] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(4U, HW_USB_sim_readTx(captured, 4U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, captured, 4U);
}

// [test->fw~hal_usb_003~1]
static void test_write_into_full_space_reports_zero(void)
{
    HW_USB_sim_setTxAccepting(false);
    const uint8_t msg[1] = { 9U };
    TEST_ASSERT_EQUAL_UINT32(0U, HW_USB_write(msg, 1U));
}

// [test->fw~hal_usb_004~1]
static void test_receive_available_and_read(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, HW_USB_available());

    const uint8_t incoming[2] = { 7U, 8U };
    HW_USB_sim_injectRx(incoming, 2U);
    TEST_ASSERT_EQUAL_UINT32(2U, HW_USB_available());

    uint8_t out[2] = { 0U };
    TEST_ASSERT_EQUAL_UINT32(2U, HW_USB_read(out, 2U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(incoming, out, 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_USB_available());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_returns_true);
    RUN_TEST(test_connection_state);
    RUN_TEST(test_write_accepts_and_reports_count);
    RUN_TEST(test_write_into_full_space_reports_zero);
    RUN_TEST(test_receive_available_and_read);
    return UNITY_END();
}
