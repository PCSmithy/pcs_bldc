#ifndef HW_TIM_H
#define HW_TIM_H

/* Includes */
#include "lib_types.h"
#include "HW_TIM_channels.h"

/* Defines */

#define HW_TIM_OC_UNITS_PER_CHANNEL  (4U)

/* Typedefs */

// Count direction. Mirror of the stm32g4 TIM_COUNTERMODE_* set, named
// target-independently so the SIM config and SIL tests need no HAL.
typedef enum
{
    HW_TIM_COUNT_UP,
    HW_TIM_COUNT_DOWN,
    HW_TIM_COUNT_CENTER,
} HW_TIM_countDir_E;

// Source event that drives the trigger output (TRGO).
typedef enum
{
    HW_TIM_TRGO_NONE,
    HW_TIM_TRGO_UPDATE,     // counter rollover (period boundary)
    HW_TIM_TRGO_OC_MATCH,   // counter reaches output-compare unit 0's value
} HW_TIM_trgoSource_E;

// One output-compare unit (mirror of the stm32g4 struct, HAL-free).
typedef struct
{
    bool     enabled;
    bool     complementary;   // models the paired CHxN line
    uint32_t compare;         // initial compare value, raw counts
    uint32_t inactiveLevel;   // output level (0/1) while disabled or idle
} HW_TIM_ocConfig_S;

// One timer peripheral. Lacks HAL handles; carries explicit scalar fields
// the stm32g4 side derives from htim.Init.
typedef struct
{
    char * channelNameStr;

    uint32_t          prescaler;
    uint32_t          period;
    uint32_t          counterWidthBits;   // 16 or 32; bounds period at init
    HW_TIM_countDir_E countDir;

    HW_TIM_ocConfig_S outputCompare[HW_TIM_OC_UNITS_PER_CHANNEL];

    bool     configureBreakDeadTime;
    uint32_t deadTime;        // dead-time generator ticks (0..255)
    bool     hasBreakInput;

    bool                configureTrgo;
    HW_TIM_trgoSource_E trgoSource;
} HW_TIM_channelConfig_S;

typedef struct
{
    const HW_TIM_channelConfig_S * channels;
    size_t numChannels;
} HW_TIM_config_S;

/* Public Function Declarations */

bool HW_TIM_init(const HW_TIM_config_S * const config);

bool HW_TIM_getCounter(HW_TIM_channels_E channel, uint32_t * const out);

bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t counts);
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t * const out);

bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit, bool enabled);

#endif // HW_TIM_H
