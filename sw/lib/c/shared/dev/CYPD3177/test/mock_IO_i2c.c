#include "IO_i2c.h"
#include "mock_IO_i2c.h"

#define MOCK_IO_I2C_MAX_ENTRIES (16U)

typedef struct
{
    bool            used;
    bool            ok;        // false => reads of this (dev, reg) fail
    IO_i2c_device_E dev;
    uint16_t        reg;
    uint8_t         bytes[MOCK_IO_I2C_MAX_BYTES];
    size_t          length;
} mockEntry_S;

static mockEntry_S     mockEntries[MOCK_IO_I2C_MAX_ENTRIES];
static bool            mockFailAll;
static size_t          mockReadCount;
static IO_i2c_device_E mockLastDevice;

// Find the entry for (dev, reg), or the first free slot, or -1 if full.
static int mock_IO_i2c_slot(IO_i2c_device_E dev, uint16_t reg)
{
    int slot      = -1;
    int firstFree = -1;
    for (size_t i = 0U; i < MOCK_IO_I2C_MAX_ENTRIES; i++)
    {
        if ((mockEntries[i].used) && (mockEntries[i].dev == dev) && (mockEntries[i].reg == reg))
        {
            slot = (int)i;
            break;
        }
        if ((firstFree < 0) && (!mockEntries[i].used))
        {
            firstFree = (int)i;
        }
    }
    return (slot >= 0) ? slot : firstFree;
}

void mock_IO_i2c_reset(void)
{
    for (size_t i = 0U; i < MOCK_IO_I2C_MAX_ENTRIES; i++)
    {
        mockEntries[i] = (mockEntry_S){ 0 };
    }
    mockFailAll    = false;
    mockReadCount  = 0U;
    mockLastDevice = IO_I2C_DEVICE_COUNT;
}

void mock_IO_i2c_setReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length)
{
    const int slot = mock_IO_i2c_slot(dev, reg);
    if (slot >= 0)
    {
        const size_t n = (length < MOCK_IO_I2C_MAX_BYTES) ? length : MOCK_IO_I2C_MAX_BYTES;
        mockEntries[slot].used   = true;
        mockEntries[slot].ok     = true;
        mockEntries[slot].dev    = dev;
        mockEntries[slot].reg    = reg;
        mockEntries[slot].length = n;
        for (size_t i = 0U; i < n; i++) { mockEntries[slot].bytes[i] = bytes[i]; }
    }
}

void mock_IO_i2c_failReg(IO_i2c_device_E dev, uint16_t reg)
{
    const int slot = mock_IO_i2c_slot(dev, reg);
    if (slot >= 0)
    {
        mockEntries[slot].used = true;
        mockEntries[slot].ok   = false;
        mockEntries[slot].dev  = dev;
        mockEntries[slot].reg  = reg;
    }
}

void mock_IO_i2c_failAll(bool fail)
{
    mockFailAll = fail;
}

size_t mock_IO_i2c_readCount(void)
{
    return mockReadCount;
}

IO_i2c_device_E mock_IO_i2c_lastDevice(void)
{
    return mockLastDevice;
}

bool IO_i2c_readReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length)
{
    mockReadCount++;
    mockLastDevice = dev;

    bool ok = true;
    if (mockFailAll)
    {
        ok = false;
    }
    else
    {
        // Zero-fill first so an entry narrower than the request (or a
        // not-injected register) reads back as clear high bytes.
        if (buffer != NULL)
        {
            for (size_t i = 0U; i < length; i++) { buffer[i] = 0U; }
        }

        for (size_t i = 0U; i < MOCK_IO_I2C_MAX_ENTRIES; i++)
        {
            if ((mockEntries[i].used) && (mockEntries[i].dev == dev) && (mockEntries[i].reg == reg))
            {
                if (!mockEntries[i].ok)
                {
                    ok = false;
                }
                else if (buffer != NULL)
                {
                    const size_t n = (length < mockEntries[i].length) ? length : mockEntries[i].length;
                    for (size_t b = 0U; b < n; b++) { buffer[b] = mockEntries[i].bytes[b]; }
                }
                else
                {
                    // matched register, but the caller passed no buffer to fill
                }
                break;
            }
        }
    }

    return ok;
}
