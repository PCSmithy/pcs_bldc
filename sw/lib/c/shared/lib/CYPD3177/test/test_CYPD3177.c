#include "CYPD3177.h"
#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/// ---- Negotiated Voltage Tests (fw~pd_001) ----

// [test->fw~pd_001~1]
static void test_voltage_field_100_decodes_to_5000mV(void) {
    TEST_ASSERT_EQUAL_UINT32(5000U, CYPD3177_negotiatedVoltage_mV(100U << CYPD3177_PDO_VOLTAGE_SHIFT));
}

// [test->fw~pd_001~1]
static void test_voltage_field_400_decodes_to_20000mV(void) {
    TEST_ASSERT_EQUAL_UINT32(20000U, CYPD3177_negotiatedVoltage_mV(400U << CYPD3177_PDO_VOLTAGE_SHIFT));
}

// [test->fw~pd_001~1]
static void test_voltage_ignores_bits_outside_field(void) {
    const uint32_t pdo = (400U << CYPD3177_PDO_VOLTAGE_SHIFT) | 0x3FFU;
    TEST_ASSERT_EQUAL_UINT32(20000U, CYPD3177_negotiatedVoltage_mV(pdo));
}

/// ---- Negotiated Current Tests (fw~pd_002) ----

// [test->fw~pd_002~1]
static void test_current_field_150_decodes_to_1500mA(void) {
    TEST_ASSERT_EQUAL_UINT32(1500U, CYPD3177_negotiatedCurrent_mA(150U << CYPD3177_RDO_CURRENT_SHIFT));
}

// [test->fw~pd_002~1]
static void test_current_field_300_decodes_to_3000mA(void) {
    TEST_ASSERT_EQUAL_UINT32(3000U, CYPD3177_negotiatedCurrent_mA(300U << CYPD3177_RDO_CURRENT_SHIFT));
}

// [test->fw~pd_002~1]
static void test_current_ignores_bits_outside_field(void) {
    const uint32_t rdo = (300U << CYPD3177_RDO_CURRENT_SHIFT) | 0x3FFU;
    TEST_ASSERT_EQUAL_UINT32(3000U, CYPD3177_negotiatedCurrent_mA(rdo));
}

/// ---- Explicit-Contract Tests (fw~pd_003) ----

// [test->fw~pd_003~1]
static void test_contract_bit_set_is_active(void) {
    TEST_ASSERT_TRUE(CYPD3177_isContractActive(1U << CYPD3177_PD_STATUS_CONTRACT_SHIFT));
}

// [test->fw~pd_003~1]
static void test_contract_bit_clear_is_inactive(void) {
    const uint32_t pdStatus = ~(1U << CYPD3177_PD_STATUS_CONTRACT_SHIFT);
    TEST_ASSERT_FALSE(CYPD3177_isContractActive(pdStatus));
}

/// ---- Bus Voltage Tests (fw~pd_004) ----

// [test->fw~pd_004~1]
static void test_bus_voltage_byte_100_decodes_to_10000mV(void) {
    TEST_ASSERT_EQUAL_UINT32(10000U, CYPD3177_busVoltage_mV(100U));
}

// [test->fw~pd_004~1]
static void test_bus_voltage_byte_200_decodes_to_20000mV(void) {
    TEST_ASSERT_EQUAL_UINT32(20000U, CYPD3177_busVoltage_mV(0xC8U));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voltage_field_100_decodes_to_5000mV);
    RUN_TEST(test_voltage_field_400_decodes_to_20000mV);
    RUN_TEST(test_voltage_ignores_bits_outside_field);
    RUN_TEST(test_current_field_150_decodes_to_1500mA);
    RUN_TEST(test_current_field_300_decodes_to_3000mA);
    RUN_TEST(test_current_ignores_bits_outside_field);
    RUN_TEST(test_contract_bit_set_is_active);
    RUN_TEST(test_contract_bit_clear_is_inactive);
    RUN_TEST(test_bus_voltage_byte_100_decodes_to_10000mV);
    RUN_TEST(test_bus_voltage_byte_200_decodes_to_20000mV);
    return UNITY_END();
}
