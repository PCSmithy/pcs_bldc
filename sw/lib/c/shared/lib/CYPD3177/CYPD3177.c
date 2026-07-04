/// INCLUDES

#include "CYPD3177.h"

/// PUBLIC FUNCTIONS

// [impl->fw~pd_003~1]
bool CYPD3177_isContractActive(uint32_t pdStatus)
{
    const bool ret = (((pdStatus >> CYPD3177_PD_STATUS_CONTRACT_SHIFT) & CYPD3177_PD_STATUS_CONTRACT_MASK) != 0U);
    return ret;
}

// [impl->fw~pd_001~1]
uint32_t CYPD3177_negotiatedVoltage_mV(uint32_t currentPdo)
{
    const uint32_t voltageField = ((currentPdo >> CYPD3177_PDO_VOLTAGE_SHIFT) & CYPD3177_PDO_VOLTAGE_MASK);
    const uint32_t ret = (voltageField * CYPD3177_PDO_VOLTAGE_SCALE_MV);
    return ret;
}

// [impl->fw~pd_002~1]
uint32_t CYPD3177_negotiatedCurrent_mA(uint32_t currentRdo)
{
    const uint32_t currentField = ((currentRdo >> CYPD3177_RDO_CURRENT_SHIFT) & CYPD3177_RDO_CURRENT_MASK);
    const uint32_t ret = (currentField * CYPD3177_RDO_CURRENT_SCALE_MA);
    return ret;
}

// [impl->fw~pd_004~1]
uint32_t CYPD3177_busVoltage_mV(uint8_t busVoltage)
{
    const uint32_t ret = (((uint32_t)busVoltage) * CYPD3177_BUS_VOLTAGE_SCALE_MV);
    return ret;
}
