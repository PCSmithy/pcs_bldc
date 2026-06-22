#ifndef LIB_TIMER_H
#define LIB_TIMER_H

/* Includes */
#include "lib_types.h"
#include "lib_utils.h"


/* Defines */

/* Typedefs */
typedef enum
{
    LIB_TIMER_PRECISION_MS,
    LIB_TIMER_PRECISION_US,
} lib_timer_precision_E;

typedef enum
{
    LIB_TIMER_STATE_UNINITIALIZED,
    LIB_TIMER_STATE_INACTIVE,
    LIB_TIMER_STATE_RUNNING,
    LIB_TIMER_STATE_EXPIRED,
} lib_timer_state_E;

typedef struct
{
    lib_timer_state_E state;
    lib_timer_precision_E precision;
    uint64_t duration;
    uint64_t startTime;
} lib_timer_channel_S;

typedef struct
{
    uint32_t (*getTime_us)(void);
} lib_timer_config_S;

/* Public Function Declarations (defined in lib_timer.c) */
// Declared ahead of the inline helpers below, which call them.
void lib_timer_run(void); // run periodically to catch overflows

uint64_t lib_timer_getTime_us(void);
uint32_t lib_timer_getTime_ms(void);

/* Static Inline Functions */
static inline void lib_timer_init(lib_timer_channel_S * timer, lib_timer_precision_E precision, uint64_t duration)
{
    if (timer != NULL)
    {
        timer->precision = precision;
        timer->duration = duration;
        timer->state = LIB_TIMER_STATE_INACTIVE;
    }
}

static inline void lib_timer_startTimer(lib_timer_channel_S * timer)
{
    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        switch (timer->precision)
        {
            default:
            case LIB_TIMER_PRECISION_MS:
                timer->startTime = lib_timer_getTime_ms();
                break;
            case LIB_TIMER_PRECISION_US:
                timer->startTime = lib_timer_getTime_us();
                break;
        }
        timer->state = LIB_TIMER_STATE_RUNNING;
    }
}

static inline void lib_timer_stopTimer(lib_timer_channel_S * timer)
{
    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        timer->state = LIB_TIMER_STATE_INACTIVE;
    }
}

static inline uint64_t lib_timer_getElapsedTime(lib_timer_channel_S * timer)
{
    uint64_t elapsed = 0U;

    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        if ((timer->state == LIB_TIMER_STATE_RUNNING) || (timer->state == LIB_TIMER_STATE_EXPIRED))
        {
            const uint64_t now = lib_timer_getTime_us();
            switch (timer->precision)
            {
                default:
                case LIB_TIMER_PRECISION_MS:
                    elapsed = US_TO_MS(now) - timer->startTime;
                    break;
                case LIB_TIMER_PRECISION_US:
                    elapsed = now - timer->startTime;
                    break;
            }
        }
    }
    return elapsed;
}

static inline uint64_t lib_timer_getRemainingTime(lib_timer_channel_S * timer)
{
    uint64_t remaining = 0U;

    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        const uint64_t elapsed = lib_timer_getElapsedTime(timer);
        remaining = timer->duration < elapsed ? 0U : (timer->duration - elapsed);
    }
    return remaining;

}

static inline lib_timer_state_E lib_timer_updateTimerAndGetState(lib_timer_channel_S * timer)
{
    lib_timer_state_E ret = LIB_TIMER_STATE_UNINITIALIZED;

    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        if (timer->state == LIB_TIMER_STATE_RUNNING)
        {
            if (lib_timer_getElapsedTime(timer) > timer->duration)
            {
                timer->state = LIB_TIMER_STATE_EXPIRED;
            }
        }
        ret = timer->state;
    }
    return ret;
}

static inline lib_timer_state_E lib_timer_runTimerWithEnable(lib_timer_channel_S * timer, bool enable)
{
    lib_timer_state_E ret = LIB_TIMER_STATE_UNINITIALIZED;

    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        if (enable)
        {
            if (timer->state == LIB_TIMER_STATE_INACTIVE) // Q: Should start from EXPIRED too?
            {
                lib_timer_startTimer(timer);
            }
        } else
        {
            lib_timer_stopTimer(timer);
        }
        ret = lib_timer_updateTimerAndGetState(timer);
    }
    return ret;
}

static inline lib_timer_state_E lib_timer_runTimerWithRestart(lib_timer_channel_S * timer, bool restart)
{
    lib_timer_state_E ret = LIB_TIMER_STATE_UNINITIALIZED;

    if ((timer != NULL) && (timer->state != LIB_TIMER_STATE_UNINITIALIZED))
    {
        if (restart)
        {
            lib_timer_startTimer(timer);
        }
        ret = lib_timer_updateTimerAndGetState(timer);
    }
    return ret;
}

#endif // LIB_TIMER_H
