#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "SIL_irq.h"
#include "SIL_irq_double.h"
#include "unity.h"

void setUp(void)
{
    // No hooks by default, so init leaves the waker handle invalid; the
    // re-entrancy test opts into the double.
    SIL_irq_setHooks(NULL);
    HW_USB_sim_reset();
    (void)HW_USB_init();
}

void tearDown(void)
{
    SIL_irq_setHooks(NULL);
}

// [test->fw~hal_usb_001~1]
static void test_init_returns_true(void)
{
    HW_USB_sim_reset();
    TEST_ASSERT_TRUE(HW_USB_init());
}

// One periodic waker per init: a second init cancels the first registration
// rather than stacking another that nothing can ever reach.
// [test->fw~hal_usb_001~1]
static void test_reinit_rewires_the_periodic_waker(void)
{
    SIL_irq_double_install(5);
    TEST_ASSERT_TRUE(HW_USB_init());
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.periodicRegisterCalls);
    TEST_ASSERT_EQUAL_UINT32(0U, SIL_irq_double.cancelCalls);

    TEST_ASSERT_TRUE(HW_USB_init());
    TEST_ASSERT_EQUAL_UINT32(1U, SIL_irq_double.cancelCalls);
    TEST_ASSERT_EQUAL_INT32(SIL_irq_double.periodicRegisterReturn,
                            SIL_irq_double.lastCancelHandle);
    TEST_ASSERT_EQUAL_UINT32(2U, SIL_irq_double.periodicRegisterCalls);
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

// [test->fw~hal_usb_005~1]
static void test_write_available_tracks_tx_space(void)
{
    const uint32_t full = HW_USB_writeAvailable();
    TEST_ASSERT_TRUE(full > 0U);

    const uint8_t msg[4] = { 1U, 2U, 3U, 4U };
    TEST_ASSERT_EQUAL_UINT32(4U, HW_USB_write(msg, 4U));
    TEST_ASSERT_EQUAL_UINT32(full - 4U, HW_USB_writeAvailable());

    HW_USB_sim_setTxAccepting(false);
    TEST_ASSERT_EQUAL_UINT32(0U, HW_USB_writeAvailable());
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
    RUN_TEST(test_reinit_rewires_the_periodic_waker);
    RUN_TEST(test_connection_state);
    RUN_TEST(test_write_accepts_and_reports_count);
    RUN_TEST(test_write_into_full_space_reports_zero);
    RUN_TEST(test_write_available_tracks_tx_space);
    RUN_TEST(test_receive_available_and_read);
    return UNITY_END();
}
