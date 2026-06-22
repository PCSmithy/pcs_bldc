#include "HW_GPIO.h"
#include "mock_HW_GPIO.h"

// A small (port, pin) -> level store. The driver only reads a handful of pins,
// so a fixed slot table keyed by (port, pin) is plenty.
#define MOCK_HW_GPIO_MAX_PINS 8U

typedef struct
{
    bool            used;
    HW_GPIO_port_E  port;
    uint32_t        pin;
    HW_GPIO_level_E level;
} mockPin_S;

static mockPin_S mockPins[MOCK_HW_GPIO_MAX_PINS];

void mock_HW_GPIO_reset(void)
{
    for (size_t i = 0U; i < MOCK_HW_GPIO_MAX_PINS; i++)
    {
        mockPins[i].used  = false;
        mockPins[i].port  = HW_GPIO_PORT_COUNT;
        mockPins[i].pin   = 0U;
        mockPins[i].level = HW_GPIO_LEVEL_LOW;
    }
}

void mock_HW_GPIO_setCachedLevel(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    int slot      = -1;
    int firstFree = -1;
    for (size_t i = 0U; i < MOCK_HW_GPIO_MAX_PINS; i++)
    {
        if ((mockPins[i].used) && (mockPins[i].port == port) && (mockPins[i].pin == pin))
        {
            slot = (int)i;
            break;
        }
        if ((firstFree < 0) && (!mockPins[i].used))
        {
            firstFree = (int)i;
        }
    }

    if (slot < 0)
    {
        slot = firstFree;
    }
    if (slot >= 0)
    {
        mockPins[slot].used  = true;
        mockPins[slot].port  = port;
        mockPins[slot].pin   = pin;
        mockPins[slot].level = level;
    }
}

HW_GPIO_level_E HW_GPIO_readCached(HW_GPIO_port_E port, uint32_t pin)
{
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    for (size_t i = 0U; i < MOCK_HW_GPIO_MAX_PINS; i++)
    {
        if ((mockPins[i].used) && (mockPins[i].port == port) && (mockPins[i].pin == pin))
        {
            level = mockPins[i].level;
            break;
        }
    }
    return level;
}
