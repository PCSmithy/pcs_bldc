/* Includes */
#include "HW_I2C.h"
#include "HW_I2C_sim.h"

/* Defines */

// Modelled devices per bus, register-offset space per device, and the largest
// byte transfer captured/injected. Tests stay well under these bounds.
#define HW_I2C_SIM_MAX_DEVICES_PER_BUS    (4U)
#define HW_I2C_SIM_REG_SPACE              (512U)
#define HW_I2C_SIM_MAX_XFER               (256U)

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
    bool    used;
    uint8_t addr7;

    uint8_t regMem[HW_I2C_SIM_REG_SPACE];   // register-addressed memory

    uint8_t lastTx[HW_I2C_SIM_MAX_XFER];    // last plain transmit() capture
    size_t  lastTxLen;

    uint8_t injectedRx[HW_I2C_SIM_MAX_XFER]; // bytes a plain receive() returns
    size_t  injectedRxLen;
} HW_I2C_simDevice_S;

typedef struct
{
    HW_I2C_simDevice_S devices[HW_I2C_SIM_MAX_DEVICES_PER_BUS];
    bool stall;
    bool forceError;
} HW_I2C_busData_S;

typedef struct
{
    const HW_I2C_config_S * config;
    bool initialized;

    HW_I2C_busData_S buses[HW_I2C_BUS_COUNT];
} HW_I2C_data_S;

/* Private Function Declarations */

static bool HW_I2C_private_busConfigValid(const HW_I2C_busConfig_S * const busConfig);
static void HW_I2C_private_clearBus(HW_I2C_bus_E bus);
static HW_I2C_simDevice_S * HW_I2C_private_getDevice(HW_I2C_bus_E bus, uint8_t addr7);
static bool HW_I2C_private_transfer(HW_I2C_bus_E bus, uint8_t devAddr7, HW_I2C_op_E op,
                                    uint16_t memAddr, uint8_t * data_, size_t length);

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

static void HW_I2C_private_clearBus(HW_I2C_bus_E bus)
{
    HW_I2C_busData_S * const busData = &data->buses[bus];
    busData->stall      = false;
    busData->forceError = false;
    for (size_t i = 0U; i < HW_I2C_SIM_MAX_DEVICES_PER_BUS; i++)
    {
        HW_I2C_simDevice_S * const dev = &busData->devices[i];
        dev->used          = false;
        dev->addr7         = 0U;
        dev->lastTxLen     = 0U;
        dev->injectedRxLen = 0U;
        for (size_t r = 0U; r < HW_I2C_SIM_REG_SPACE; r++)
        {
            dev->regMem[r] = 0U;
        }
    }
}

// Find the device slot for `addr7`, allocating a free one on first contact.
// Returns NULL only when the bus already tracks the maximum device count.
static HW_I2C_simDevice_S * HW_I2C_private_getDevice(HW_I2C_bus_E bus, uint8_t addr7)
{
    HW_I2C_simDevice_S * found = NULL;
    HW_I2C_busData_S * const busData = &data->buses[bus];

    for (size_t i = 0U; i < HW_I2C_SIM_MAX_DEVICES_PER_BUS; i++)
    {
        if ((busData->devices[i].used) && (busData->devices[i].addr7 == addr7))
        {
            found = &busData->devices[i];
        }
    }

    if (found == NULL)
    {
        for (size_t i = 0U; i < HW_I2C_SIM_MAX_DEVICES_PER_BUS; i++)
        {
            if ((found == NULL) && (!busData->devices[i].used))
            {
                busData->devices[i].used  = true;
                busData->devices[i].addr7 = addr7;
                found = &busData->devices[i];
            }
        }
    }

    return found;
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
// [impl->fw~hal_i2c_004~1]
static bool HW_I2C_private_transfer(HW_I2C_bus_E bus, uint8_t devAddr7, HW_I2C_op_E op,
                                    uint16_t memAddr, uint8_t * data_, size_t length)
{
    bool ret = false;
    if ((data->initialized) &&
        (bus < HW_I2C_BUS_COUNT) &&
        (data->config->buses[bus].enabled) &&
        (length > 0U) &&
        (data_ != NULL))
    {
        HW_I2C_busData_S * const busData = &data->buses[bus];
        HW_I2C_simDevice_S * const dev = HW_I2C_private_getDevice(bus, devAddr7);
        const bool fault = (busData->stall) || (busData->forceError);

        if ((dev != NULL) && (!fault))
        {
            const size_t regEnd = (size_t)memAddr + length;
            switch (op)
            {
                case HW_I2C_OP_TX:
                {
                    const size_t n = (length < HW_I2C_SIM_MAX_XFER) ? length : HW_I2C_SIM_MAX_XFER;
                    for (size_t i = 0U; i < n; i++)
                    {
                        dev->lastTx[i] = data_[i];
                    }
                    dev->lastTxLen = length;
                    ret = true;
                    break;
                }
                case HW_I2C_OP_RX:
                {
                    for (size_t i = 0U; i < length; i++)
                    {
                        data_[i] = (i < dev->injectedRxLen) ? dev->injectedRx[i] : 0U;
                    }
                    ret = true;
                    break;
                }
                case HW_I2C_OP_MEM_WRITE:
                {
                    if (regEnd <= HW_I2C_SIM_REG_SPACE)
                    {
                        for (size_t i = 0U; i < length; i++)
                        {
                            dev->regMem[(size_t)memAddr + i] = data_[i];
                        }
                        ret = true;
                    }
                    break;
                }
                case HW_I2C_OP_MEM_READ:
                {
                    if (regEnd <= HW_I2C_SIM_REG_SPACE)
                    {
                        for (size_t i = 0U; i < length; i++)
                        {
                            data_[i] = dev->regMem[(size_t)memAddr + i];
                        }
                        ret = true;
                    }
                    break;
                }
                default:
                    ret = false;
                    break;
            }
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
        for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
        {
            success &= HW_I2C_private_busConfigValid(&config->buses[bus]);
        }

        if (success)
        {
            data->config = config;
            for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
            {
                HW_I2C_private_clearBus(bus);
            }
            data->initialized = true;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
bool HW_I2C_transmit(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_TX, 0U, data_, length);
}

// [impl->fw~hal_i2c_002~1]
// [impl->fw~hal_i2c_003~1]
bool HW_I2C_receive(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data_, size_t length)
{
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_RX, 0U, data_, length);
}

// [impl->fw~hal_i2c_004~1]
bool HW_I2C_memRead(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                    HW_I2C_memAddrSize_E memAddrSize, uint8_t * data_, size_t length)
{
    // Offset width does not change how the model stores bytes; it affects only
    // the on-wire address phase and the timeout byte count on real hardware.
    (void)memAddrSize;
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_MEM_READ, memAddr, data_, length);
}

// [impl->fw~hal_i2c_004~1]
bool HW_I2C_memWrite(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                     HW_I2C_memAddrSize_E memAddrSize, uint8_t * data_, size_t length)
{
    (void)memAddrSize;
    return HW_I2C_private_transfer(bus, devAddr7, HW_I2C_OP_MEM_WRITE, memAddr, data_, length);
}

/* SIL control + inspection (HW_I2C_sim.h) */

void HW_I2C_sim_reset(void)
{
    for (HW_I2C_bus_E bus = 0U; bus < HW_I2C_BUS_COUNT; bus++)
    {
        HW_I2C_private_clearBus(bus);
    }
}

void HW_I2C_sim_setInjectedRx(HW_I2C_bus_E bus, uint8_t devAddr7, const uint8_t * bytes, size_t length)
{
    if ((bus < HW_I2C_BUS_COUNT) && (bytes != NULL))
    {
        HW_I2C_simDevice_S * const dev = HW_I2C_private_getDevice(bus, devAddr7);
        if (dev != NULL)
        {
            const size_t copyLen = (length < HW_I2C_SIM_MAX_XFER) ? length : HW_I2C_SIM_MAX_XFER;
            for (size_t i = 0U; i < copyLen; i++)
            {
                dev->injectedRx[i] = bytes[i];
            }
            dev->injectedRxLen = copyLen;
        }
    }
}

size_t HW_I2C_sim_getLastTx(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * out, size_t maxLength)
{
    size_t txLen = 0U;
    if ((bus < HW_I2C_BUS_COUNT) && (out != NULL))
    {
        const HW_I2C_simDevice_S * const dev = HW_I2C_private_getDevice(bus, devAddr7);
        if (dev != NULL)
        {
            txLen = dev->lastTxLen;
            const size_t captured = (dev->lastTxLen < HW_I2C_SIM_MAX_XFER) ? dev->lastTxLen : HW_I2C_SIM_MAX_XFER;
            const size_t copyLen  = (captured < maxLength) ? captured : maxLength;
            for (size_t i = 0U; i < copyLen; i++)
            {
                out[i] = dev->lastTx[i];
            }
        }
    }
    return txLen;
}

void HW_I2C_sim_setRegBytes(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                            const uint8_t * bytes, size_t length)
{
    if ((bus < HW_I2C_BUS_COUNT) && (bytes != NULL) && (((size_t)memAddr + length) <= HW_I2C_SIM_REG_SPACE))
    {
        HW_I2C_simDevice_S * const dev = HW_I2C_private_getDevice(bus, devAddr7);
        if (dev != NULL)
        {
            for (size_t i = 0U; i < length; i++)
            {
                dev->regMem[(size_t)memAddr + i] = bytes[i];
            }
        }
    }
}

size_t HW_I2C_sim_getRegBytes(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                              uint8_t * out, size_t length)
{
    size_t copied = 0U;
    if ((bus < HW_I2C_BUS_COUNT) && (out != NULL) && (((size_t)memAddr + length) <= HW_I2C_SIM_REG_SPACE))
    {
        const HW_I2C_simDevice_S * const dev = HW_I2C_private_getDevice(bus, devAddr7);
        if (dev != NULL)
        {
            for (size_t i = 0U; i < length; i++)
            {
                out[i] = dev->regMem[(size_t)memAddr + i];
            }
            copied = length;
        }
    }
    return copied;
}

void HW_I2C_sim_setStall(HW_I2C_bus_E bus, bool stall)
{
    if (bus < HW_I2C_BUS_COUNT)
    {
        data->buses[bus].stall = stall;
    }
}

void HW_I2C_sim_setForceError(HW_I2C_bus_E bus, bool forceError)
{
    if (bus < HW_I2C_BUS_COUNT)
    {
        data->buses[bus].forceError = forceError;
    }
}
