#pragma once

// Target-specific half of HW_TIM; reached via HW_TIM.h.

/* Includes */
#include "lib_types.h"

#include "HW_TIM_channels.h"

/* Typedefs */

// Count direction, named target-independently so the sim config and SIL tests
// need no HAL.
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

// One timer peripheral. Lacks HAL handles; carries explicit scalar fields
// the stm32g4 target derives from htim.Init.
typedef struct
{
    char * nameStr;

    uint32_t          prescaler;
    uint32_t          period;
    uint32_t          counterWidthBits;   // 16 or 32; bounds period at init
    HW_TIM_countDir_E countDir;
    uint32_t          countsPerUs;        // counter counts per sim microsecond
                                          // (0 = counter does not track sim time)

    bool     configureBreakDeadTime;
    uint32_t deadTime;        // dead-time generator ticks (0..255)
    bool     hasBreakInput;

    bool                configureTrgo;
    HW_TIM_trgoSource_E trgoSource;
} HW_TIM_peripheralConfig_S;

// One logical channel: an output-compare unit on a named peripheral.
typedef struct
{
    HW_TIM_peripheral_E  peripheral;    // peripheral this channel lives on
    HW_TIM_channelRole_E role;          // HW_TIM_ROLE_OUTPUT_COMPARE
    uint8_t              ocUnit;        // dense OC-unit index 0..3
    bool                 complementary; // models the paired CHxN line
    uint32_t             compare;       // initial compare value, raw counts
    uint32_t             inactiveLevel; // output level (0/1) while disabled or idle
    char *               channelNameStr; // SIL port base name; NULL registers no ports
} HW_TIM_channelConfig_S;

/* Public Function Declarations */

// Advance sim time: each peripheral with countsPerUs > 0 advances its counter
// by elapsed_us * countsPerUs, wrapping modulo (period + 1) in its count
// direction. The platform tick calls this once per sim tick — it is the clock
// behind the timebase peripherals (lib_timer's 1 us source rides TIM2).
void HW_TIM_advanceTime(uint32_t elapsed_us);
