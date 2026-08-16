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
    HW_TIM_TRGO_OC_MATCH,   // counter reaches trgoOcUnit's compare value
} HW_TIM_trgoSource_E;

// Invoked once per trigger event a peripheral emits. `context` is the pointer
// supplied to HW_TIM_registerTrgoCallback.
typedef void (*HW_TIM_trgoCallback_F)(HW_TIM_peripheral_E peripheral, void * context);

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
    uint32_t          rcr;                // repetition counter: reload events per
                                          // update event, minus one (0 = every reload)

    bool     configureBreakDeadTime;
    uint32_t deadTime;        // dead-time generator ticks (0..255)
    bool     hasBreakInput;

    bool                configureTrgo;
    HW_TIM_trgoSource_E trgoSource;
    uint8_t             trgoOcUnit;   // OC unit behind an OC-match source
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
// by elapsed_us * countsPerUs. An up- or down-counter walks period + 1 counts
// per cycle; a center-aligned one walks 2 * period, up to the period and back
// down. The platform tick calls this once per sim tick — it is the clock behind
// the timebase peripherals (lib_timer's 1 us source rides TIM2).
//
// A peripheral with a trigger output configured emits one trigger per source
// event, so an advance spanning several cycles emits several. An update source
// fires once per rcr + 1 reload events (a center-aligned counter reloads at
// both extremes); an OC-match source fires on each landing on the compare
// value. Sinks run after the counter has taken its end-of-advance value.
void HW_TIM_advanceTime(uint32_t elapsed_us);

// Register the sink for a peripheral's trigger output. Only sim-target
// hw-layer code registers — the consumer is the sim ADC's hardware-triggered
// conversion. One sink per peripheral: the modelled hardware routes a trigger
// to a single consumer, so a later registration replaces the earlier one and a
// NULL callback clears it. Independent of HW_TIM_init — the two run in either
// order and a re-init keeps the registration, so a firmware restart re-wires
// rather than orphans. Returns false if the peripheral is out of range.
bool HW_TIM_registerTrgoCallback(HW_TIM_peripheral_E peripheral,
                                 HW_TIM_trgoCallback_F callback,
                                 void * context);
