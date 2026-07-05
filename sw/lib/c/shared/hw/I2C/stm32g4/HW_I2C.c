/* Includes */
#include "HW_I2C.h"
#include "HW_I2C_timeout.h"
#include "stm32g4xx_hal.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* Defines */

// I2C event/error IRQs run at the FreeRTOS max-syscall priority (matches the
// DMA path), so their completion callbacks may use the FromISR API.
#define HW_I2C_IRQ_PRIORITY    (5U)

// Bounded wait for an issued abort to settle (AbortCpltCallback gives the
// semaphore). An abort is a STOP condition plus state teardown — milliseconds
// at any supported bit rate.
#define HW_I2C_ABORT_SETTLE_MS (5U)

/* Typedefs */
typedef enum
{
    HW_I2C_OP_TX,
    HW_I2C_OP_RX,
    HW_I2C_OP_MEM_READ,
    HW_I2C_OP_MEM_WRITE,
} HW_I2C_op_E;

typedef struct
{
    I2C_HandleTypeDef hi2c;

    // Calling task blocks on this until a completion/error/abort callback
    // signals it (or the take times out). One in-flight transfer per bus.
    SemaphoreHandle_t doneSem;
    StaticSemaphore_t doneSemBuffer;
    volatile bool     transferOk;
} HW_I2C_busData_S;

typedef struct
{
    const HW_I2C_config_S * config;
    bool initialized;

    HW_I2C_busData_S buses[HW_I2C_BUS_COUNT];
} HW_I2C_data_S;

/* Private Function Declarations */

static bool HW_I2C_private_busConfigValid(const HW_I2C_busConfig_S * const busConfig);
static void HW_I2C_private_enableIrqs(const I2C_TypeDef * const instance);
static void HW_I2C_private_signalDone(const I2C_HandleTypeDef * const hi2c, bool ok);
static bool HW_I2C_private_waitDone(HW_I2C_bus_E bus, uint8_t devAddr7, uint32_t timeoutMs);
static bool HW_I2C_private_transfer(HW_I2C_bus_E bus, uint8_t devAddr7, HW_I2C_op_E op,
                                    uint16_t memAddr, HW_I2C_memAddrSize_E memAddrSize,
                                    uint8_t * data_, size_t length);

/* Private Data Definitions */

static HW_I2C_data_S HW_I2C_data;
static HW_I2C_data_S * const data = &HW_I2C_data;

/* Private Function Definitions */

// [impl->fw~hal_i2c_001~1]
static bool HW_I2C_private_busConfigValid(const HW_I2C_busConfig_S * const busConfig)
{
    bool valid = true;
    if (busConfig->enabled)
    {
        // Only interrupt mode is implemented; DMA is reserved but unsupported.
        valid = (busConfig->transferMode == HW_I2C_TRANSFERMODE_INTERRUPT);
    }
    return valid;
}

static void HW_I2C_private_enableIrqs(const I2C_TypeDef * const instance)
{
    IRQn_Type evIrq = I2C1_EV_IRQn;
    IRQn_Type erIrq = I2C1_ER_IRQn;
    bool known = true;

    if (instance == I2C1)
    {
        evIrq = I2C1_EV_IRQn;
        erIrq = I2C1_ER_IRQn;
    }
    else if (instance == I2C2)
    {
        evIrq = I2C2_EV_IRQn;
        erIrq = I2C2_ER_IRQn;
    }
    else if (instance == I2C3)
    {
        evIrq = I2C3_EV_IRQn;
        erIrq = I2C3_ER_IRQn;
    }
    else
    {
        known = false;
    }

    if (known)
    {
        HAL_NVIC_SetPriority(evIrq, HW_I2C_IRQ_PRIORITY, 0U);
        HAL_NVIC_EnableIRQ(evIrq);
        HAL_NVIC_SetPriority(erIrq, HW_I2C_IRQ_PRIORITY, 0U);
        HAL_NVIC_EnableIRQ(erIrq);
    }
}

// Signal the bus owning `hi2c` that its transfer settled. One weak-callback set
// serves every I2C instance, so dispatch is by matching the peripheral.
static void HW_I2C_private_signalDone(const I2C_HandleTypeDef * const hi2c, bool ok)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
    {
        if (data->buses[bus].hi2c.Instance == hi2c->Instance)
        {
            data->buses[bus].transferOk = ok;
            (void)xSemaphoreGiveFromISR(data->buses[bus].doneSem, &higherPriorityTaskWoken);
            break;
        }
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

// [impl->fw~hal_i2c_003~1]
static bool HW_I2C_private_waitDone(HW_I2C_bus_E bus, uint8_t devAddr7, uint32_t timeoutMs)
{
    bool ret = false;
    HW_I2C_busData_S * const busData = &data->buses[bus];
    if (xSemaphoreTake(busData->doneSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE)
    {
        ret = busData->transferOk;
    }
    else
    {
        // Timed out: tear the in-flight transfer down so the bus is usable
        // again, then wait (bounded) for the abort to settle. Without the wait
        // the abort completes after the next transfer starts, and that transfer
        // fails spuriously with HAL_BUSY on a healthy bus.
        const uint16_t halAddr = (uint16_t)((uint32_t)devAddr7 << 1U);
        (void)HAL_I2C_Master_Abort_IT(&busData->hi2c, halAddr);
        (void)xSemaphoreTake(busData->doneSem, pdMS_TO_TICKS(HW_I2C_ABORT_SETTLE_MS));
    }
    return ret;
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
// [impl->fw~hal_i2c_004~1]
static bool HW_I2C_private_transfer(HW_I2C_bus_E bus, uint8_t devAddr7, HW_I2C_op_E op,
                                    uint16_t memAddr, HW_I2C_memAddrSize_E memAddrSize,
                                    uint8_t * data_, size_t length)
{
    bool ret = false;
    if ((data->initialized) &&
        (bus < HW_I2C_BUS_COUNT) &&
        (data->config->buses[bus].enabled) &&
        (length > 0U) &&
        (data_ != NULL))
    {
        HW_I2C_busData_S * const busData = &data->buses[bus];
        const HW_I2C_busConfig_S * const busConfig = &data->config->buses[bus];
        const uint16_t halAddr = (uint16_t)((uint32_t)devAddr7 << 1U);

        // Register offset bytes count toward N in the timeout formula.
        const bool isMemOp    = ((op == HW_I2C_OP_MEM_READ) || (op == HW_I2C_OP_MEM_WRITE));
        const bool is16BitAddr = (memAddrSize != HW_I2C_MEMADDR_SIZE_8BIT);
        const size_t addrBytes = (is16BitAddr) ? 2U : 1U;
        const size_t numBytes  = (isMemOp) ? (length + addrBytes) : length;
        const uint32_t halMemSize = (is16BitAddr) ? (uint32_t)I2C_MEMADD_SIZE_16BIT
                                                  : (uint32_t)I2C_MEMADD_SIZE_8BIT;
        // The HAL transmits a 16-bit offset MSB-first; for an LSB-first device
        // (e.g. Cypress HPI) pre-swap the bytes so the wire order comes out low
        // byte first.
        const uint16_t halMemAddr = (memAddrSize == HW_I2C_MEMADDR_SIZE_16BIT_LSBFIRST)
                                        ? (uint16_t)(((memAddr & 0x00FFU) << 8U) | (memAddr >> 8U))
                                        : memAddr;
        const uint32_t timeoutMs = HW_I2C_computeTimeoutMs(busConfig->sclBitRateHz, numBytes);

        // Drop any stale completion token from a prior aborted transfer so this
        // transfer waits on its own signal.
        (void)xSemaphoreTake(busData->doneSem, 0U);
        busData->transferOk = false;

        HAL_StatusTypeDef halStatus = HAL_ERROR;
        switch (op)
        {
            case HW_I2C_OP_TX:
                halStatus = HAL_I2C_Master_Transmit_IT(&busData->hi2c, halAddr, data_, (uint16_t)length);
                break;
            case HW_I2C_OP_RX:
                halStatus = HAL_I2C_Master_Receive_IT(&busData->hi2c, halAddr, data_, (uint16_t)length);
                break;
            case HW_I2C_OP_MEM_READ:
                halStatus = HAL_I2C_Mem_Read_IT(&busData->hi2c, halAddr, halMemAddr, halMemSize, data_, (uint16_t)length);
                break;
            case HW_I2C_OP_MEM_WRITE:
                halStatus = HAL_I2C_Mem_Write_IT(&busData->hi2c, halAddr, halMemAddr, halMemSize, data_, (uint16_t)length);
                break;
            default:
                halStatus = HAL_ERROR;
                break;
        }

        if (halStatus == HAL_OK)
        {
            ret = HW_I2C_private_waitDone(bus, devAddr7, timeoutMs);
        }
    }
    return ret;
}

/* Public Function Definitions */

// [impl->fw~hal_i2c_001~1]
bool HW_I2C_init(const HW_I2C_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        bool success = true;

        // Validate every bus before touching hardware so a bad config fails
        // init cleanly rather than half-configuring a peripheral.
        for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
        {
            success &= HW_I2C_private_busConfigValid(&config->buses[bus]);
        }

        if (success)
        {
            ret = true;

            for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
            {
                if (config->buses[bus].enabled)
                {
                    data->buses[bus].hi2c       = config->buses[bus].hi2c;
                    data->buses[bus].transferOk = false;
                    data->buses[bus].doneSem    =
                        xSemaphoreCreateBinaryStatic(&data->buses[bus].doneSemBuffer);
                    ret &= (data->buses[bus].doneSem != NULL);

                    ret &= (HAL_I2C_Init(&data->buses[bus].hi2c) == HAL_OK);
                    ret &= (HAL_I2CEx_ConfigAnalogFilter(&data->buses[bus].hi2c,
                                                         I2C_ANALOGFILTER_ENABLE) == HAL_OK);
                    ret &= (HAL_I2CEx_ConfigDigitalFilter(&data->buses[bus].hi2c, 0U) == HAL_OK);

                    // The generated MSP wires the pins/clock but not the NVIC
                    // event/error lines the interrupt-mode transfers need.
                    HW_I2C_private_enableIrqs(config->buses[bus].hi2c.Instance);
                }
            }

            data->config      = config;
            data->initialized = ret;
        }
    }
    return ret;
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
bool HW_I2C_transmit(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_TX, 0U, HW_I2C_MEMADDR_SIZE_8BIT, data_, length);
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
bool HW_I2C_receive(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_RX, 0U, HW_I2C_MEMADDR_SIZE_8BIT, data_, length);
}

// [impl->fw~hal_i2c_004~1]
bool HW_I2C_memRead(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                    HW_I2C_memAddrSize_E memAddrSize, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_MEM_READ, memAddr, memAddrSize, data_, length);
}

// [impl->fw~hal_i2c_004~1]
bool HW_I2C_memWrite(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                     HW_I2C_memAddrSize_E memAddrSize, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_MEM_WRITE, memAddr, memAddrSize, data_, length);
}

void HW_I2C_irqHandlerEv(HW_I2C_bus_E bus)
{
    if (bus < HW_I2C_BUS_COUNT)
    {
        HAL_I2C_EV_IRQHandler(&data->buses[bus].hi2c);
    }
}

void HW_I2C_irqHandlerEr(HW_I2C_bus_E bus)
{
    if (bus < HW_I2C_BUS_COUNT)
    {
        HAL_I2C_ER_IRQHandler(&data->buses[bus].hi2c);
    }
}

/* HAL completion callbacks — one weak set shared by every I2C instance. */
// [impl->fw~hal_i2c_003~1]
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, true);
}

// [impl->fw~hal_i2c_003~1]
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, true);
}

// [impl->fw~hal_i2c_004~1]
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, true);
}

// [impl->fw~hal_i2c_004~1]
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, true);
}

// [impl->fw~hal_i2c_003~1]
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, false);
}

// [impl->fw~hal_i2c_003~1]
void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef * hi2c)
{
    HW_I2C_private_signalDone(hi2c, false);
}
