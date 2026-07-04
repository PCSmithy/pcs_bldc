/* Includes */
#include "IO_i2c.h"
#include "HW_I2C.h"

#include "IO_i2c_channels.h"

/* Defines */

typedef struct
{
    const IO_i2c_config_S * config;
} IO_i2c_data_S;

static IO_i2c_data_S IO_i2c_data;
static IO_i2c_data_S * const data = &IO_i2c_data;

/* Public Function Definitions */

// [impl->fw~io_i2c_001~1]
bool IO_i2c_init(const IO_i2c_config_S * const config)
{
    bool success = false;

    if ((config != NULL) &&
        (config->devices != NULL) &&
        (config->numDevices <= IO_I2C_DEVICE_COUNT))
    {
        success = true;
        for (size_t dev = 0U; dev < config->numDevices; dev++)
        {
            if (config->devices[dev].bus >= HW_I2C_BUS_COUNT)
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->config = config;
        }
    }

    return success;
}

// [impl->fw~io_i2c_002~1] [impl->fw~io_i2c_003~1]
bool IO_i2c_readReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length)
{
    bool ret = false;
    if ((data->config != NULL) && (dev < IO_I2C_DEVICE_COUNT))
    {
        const IO_i2c_deviceConfig_S * const deviceConfig = &data->config->devices[dev];
        ret = HW_I2C_memRead(deviceConfig->bus, deviceConfig->devAddr7, reg,
                             deviceConfig->memAddrSize, buffer, length);
    }
    return ret;
}

// [impl->fw~io_i2c_002~1] [impl->fw~io_i2c_003~1]
bool IO_i2c_writeReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length)
{
    bool ret = false;
    if ((data->config != NULL) && (dev < IO_I2C_DEVICE_COUNT))
    {
        const IO_i2c_deviceConfig_S * const deviceConfig = &data->config->devices[dev];
        ret = HW_I2C_memWrite(deviceConfig->bus, deviceConfig->devAddr7, reg,
                              deviceConfig->memAddrSize, buffer, length);
    }
    return ret;
}
