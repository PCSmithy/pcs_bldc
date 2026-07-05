#include "IO_i2c.h"
#include "mock_IO_i2c.h"

#define MOCK_IO_I2C_MAX_ENTRIES (16U)

typedef struct
{
    bool            used;
    bool            readOk;    // false => reads of this (dev, reg) fail
    bool            writeOk;   // false => writes to this (dev, reg) fail
    bool            sticky;    // true => writes succeed but store nothing
    IO_i2c_device_E dev;
    uint16_t        reg;
    uint8_t         bytes[MOCK_IO_I2C_MAX_BYTES];
    size_t          length;
} mockEntry_S;

static mockEntry_S         mockEntries[MOCK_IO_I2C_MAX_ENTRIES];
static bool                mockFailAll;
static size_t              mockReadCount;
static size_t              mockWriteCount;
static mock_IO_i2c_write_S mockWrites[MOCK_IO_I2C_MAX_WRITES];
static size_t              mockWriteLogLen;

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

static mockEntry_S * mock_IO_i2c_claim(IO_i2c_device_E dev, uint16_t reg)
{
    mockEntry_S * entry = NULL;
    const int slot = mock_IO_i2c_slot(dev, reg);
    if (slot >= 0)
    {
        entry = &mockEntries[slot];
        if (!entry->used)
        {
            entry->used    = true;
            entry->readOk  = true;
            entry->writeOk = true;
            entry->sticky  = false;
            entry->dev     = dev;
            entry->reg     = reg;
            entry->length  = 0U;
        }
    }
    return entry;
}

void mock_IO_i2c_reset(void)
{
    for (size_t i = 0U; i < MOCK_IO_I2C_MAX_ENTRIES; i++)
    {
        mockEntries[i] = (mockEntry_S){ 0 };
    }
    mockFailAll     = false;
    mockReadCount   = 0U;
    mockWriteCount  = 0U;
    mockWriteLogLen = 0U;
}

void mock_IO_i2c_setReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length)
{
    mockEntry_S * const entry = mock_IO_i2c_claim(dev, reg);
    if (entry != NULL)
    {
        const size_t n = (length < MOCK_IO_I2C_MAX_BYTES) ? length : MOCK_IO_I2C_MAX_BYTES;
        entry->length = n;
        for (size_t i = 0U; i < n; i++) { entry->bytes[i] = bytes[i]; }
    }
}

void mock_IO_i2c_failReg(IO_i2c_device_E dev, uint16_t reg)
{
    mockEntry_S * const entry = mock_IO_i2c_claim(dev, reg);
    if (entry != NULL)
    {
        entry->readOk = false;
    }
}

void mock_IO_i2c_failWriteReg(IO_i2c_device_E dev, uint16_t reg)
{
    mockEntry_S * const entry = mock_IO_i2c_claim(dev, reg);
    if (entry != NULL)
    {
        entry->writeOk = false;
    }
}

void mock_IO_i2c_stickReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length)
{
    mock_IO_i2c_setReg(dev, reg, bytes, length);
    mockEntry_S * const entry = mock_IO_i2c_claim(dev, reg);
    if (entry != NULL)
    {
        entry->sticky = true;
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

size_t mock_IO_i2c_writeCount(void)
{
    return mockWriteCount;
}

size_t mock_IO_i2c_getWrites(mock_IO_i2c_write_S * out, size_t maxWrites)
{
    if (out != NULL)
    {
        const size_t n = (mockWriteLogLen < maxWrites) ? mockWriteLogLen : maxWrites;
        for (size_t i = 0U; i < n; i++) { out[i] = mockWrites[i]; }
    }
    return mockWriteLogLen;
}

bool IO_i2c_readReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length)
{
    mockReadCount++;

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
                if (!mockEntries[i].readOk)
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

bool IO_i2c_writeReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length)
{
    mockWriteCount++;
    if ((mockWriteLogLen < MOCK_IO_I2C_MAX_WRITES) && (buffer != NULL) && (length > 0U))
    {
        mockWrites[mockWriteLogLen] = (mock_IO_i2c_write_S){ .dev = dev, .reg = reg, .value = buffer[0] };
        mockWriteLogLen++;
    }

    bool ok = true;
    if ((mockFailAll) || (buffer == NULL))
    {
        ok = false;
    }
    else
    {
        mockEntry_S * const entry = mock_IO_i2c_claim(dev, reg);
        if (entry == NULL)
        {
            ok = false;
        }
        else if (!entry->writeOk)
        {
            ok = false;
        }
        else if (!entry->sticky)
        {
            const size_t n = (length < MOCK_IO_I2C_MAX_BYTES) ? length : MOCK_IO_I2C_MAX_BYTES;
            entry->length = n;
            for (size_t i = 0U; i < n; i++) { entry->bytes[i] = buffer[i]; }
        }
        else
        {
            // sticky: report success, keep the seeded bytes
        }
    }

    return ok;
}
