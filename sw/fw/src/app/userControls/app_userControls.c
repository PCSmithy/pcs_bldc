/* Includes */
#include <math.h>

#include "lib_utils.h"
#include "lib_timer.h"

#include "app_userControls.h"
#include "app_motorControl.h"

/* Defines */

// Dial travel (degrees of accumulated rotation) that maps to full scale in
// either direction: half a turn from zero pegs the command at +/-1.
#define APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG (180.0f)

// Button hold (ms) that triggers the fault-clear gesture (fw~mc_007). A press
// released before this toggles run/stop; a hold of this long clears a latched
// motor fault (and never also toggles).
#define APP_USERCONTROLS_FAULT_CLEAR_HOLD_MS (3000U)

// A velocity write counts as a dial change only past this deadband from the
// last SENT value, so encoder LSB noise can't re-assert the dial every cycle
// against an app command (fw~conn_server_002 arbitration).
#define APP_USERCONTROLS_VELOCITY_SEND_DEADBAND_RAD_PER_SEC (0.05f)

// Window (ms) after a tap to wait for a second tap: a double-tap cycles the LED
// ring's display mode, a lone tap (window elapsed) toggles run/stop.
#define APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS (300U)

/* Typedefs */

typedef struct
{
    const app_userControls_config_S * config;
    app_userControls_mode_E mode;

    float32_t dialAngleRaw_deg;

    // Sticky signed dial accumulator (push this process to a future
    // dev_encoder): wrapped raw deltas accumulate into dialAccum_deg, clamped
    // to +/-APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG; dialCommand is that
    // accumulator normalized to [-1, +1].
    float32_t dialPrevRaw_deg;
    float32_t dialAccum_deg;
    float32_t dialCommand;
    bool isButtonPressedPrev;

    // Button-gesture recognition. holdTimer distinguishes a tap from a long
    // hold (fault-clear); gestureConsumed marks a hold that already fired so its
    // release does not also tap. tapWindow defers a lone tap until it is clear
    // no second tap follows (double-tap cycles the ring instead of toggling).
    lib_timer_channel_S holdTimer;
    bool gestureConsumed;
    lib_timer_channel_S tapWindow;
    bool tapPending;

    float32_t velocityRequest_radPerSec;

    // Last mode/velocity actually written to motorControl. Outputs are written
    // only on change, so an app command (the other writer of the same request
    // state) is not re-overwritten every cycle — later writer prevails.
    app_motorControl_mode_E modeSent;
    float32_t velocitySent_radPerSec;
    bool outputsSeeded;   // first cycle always writes
} app_userControls_data_S;

/* Private Function Declarations */
static float32_t app_userControls_private_readAngleDeg(IO_AS5048_channel_E channel);
static float32_t app_userControls_private_dialAccumulate(float32_t * const accum_deg,
                                                         float32_t * const prevRaw_deg,
                                                         const float32_t raw_deg);

/* Private Data Definitions */
static app_userControls_data_S app_userControls_data;
static app_userControls_data_S * const data = &app_userControls_data;

/* Private Function Definitions */
static float32_t app_userControls_private_readAngleDeg(IO_AS5048_channel_E channel)
{
    float32_t deg = 0.0f;
    (void)IO_AS5048_readAngle(channel, NULL, &deg, NULL);
    return deg;
}

// Fold one cycle's dial movement into a sticky, clamped, signed accumulator
// and return the command normalized to [-1, +1]. Adapted from
// app_rgbLedRing's walkAdvance: the raw-angle delta is wrapped across the
// 0/360 seam so multi-turn winding accumulates, and the accumulator pegs at
// the rails until wound back the other way.
// [impl->fw~mc_008~1]
static float32_t app_userControls_private_dialAccumulate(float32_t * const accum_deg,
                                                         float32_t * const prevRaw_deg,
                                                         const float32_t raw_deg)
{
    float32_t delta_deg = raw_deg - *prevRaw_deg;
    if (delta_deg > 180.0f)
    {
        delta_deg -= 360.0f;
    }
    else if (delta_deg < -180.0f)
    {
        delta_deg += 360.0f;
    }
    *prevRaw_deg = raw_deg;

    *accum_deg += delta_deg;
    if (*accum_deg > APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG)
    {
        *accum_deg = APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG;
    }
    if (*accum_deg < -APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG)
    {
        *accum_deg = -APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG;
    }

    return *accum_deg / APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG;
}


/* Public Function Definitions */

bool app_userControls_init(const app_userControls_config_S * const config)
{
    bool success = false;
    if (config != NULL)
    {
        success = true;
        success &= (config->motor < APP_MOTORCONTROL_CHANNEL_COUNT);
        success &= (config->button < DEV_SWITCH_CHANNEL_COUNT);
        success &= (config->dial < IO_AS5048_CHANNEL_COUNT);
        success &= (config->ring < APP_RGBLEDRING_CHANNEL_COUNT);

        if (success)
        {
            data->config = config;
            lib_timer_init(&data->holdTimer, LIB_TIMER_PRECISION_MS, APP_USERCONTROLS_FAULT_CLEAR_HOLD_MS);
            lib_timer_init(&data->tapWindow, LIB_TIMER_PRECISION_MS, APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS);
            data->gestureConsumed = false;
            data->tapPending = false;
        }
    }
    return success;
}

void app_userControls_run1ms(void)
{
    if (data->config != NULL)
    {
        // fetch inputs
        data->dialAngleRaw_deg = app_userControls_private_readAngleDeg(data->config->dial);

        // button gesture recognition
        const bool isButtonPressed = dev_switch_isActive(data->config->button);
        const bool isButtonPressedRisingEdge = isButtonPressed && !data->isButtonPressedPrev;
        const bool isButtonReleasedEdge = (!isButtonPressed) && data->isButtonPressedPrev;
        data->isButtonPressedPrev = isButtonPressed;

        if (isButtonPressedRisingEdge)
        {
            data->gestureConsumed = false;
        }

        // Long hold -> fault-clear (only when a fault is latched). The hold timer
        // runs while the button is down; on expiry the gesture is consumed so the
        // release below cannot also toggle. A hold with no fault to clear still
        // consumes (no action), matching "gesture outside its context does nothing".
        const lib_timer_state_E holdState = lib_timer_runTimerWithEnable(&data->holdTimer, isButtonPressed);
        if ((holdState == LIB_TIMER_STATE_EXPIRED) && (!data->gestureConsumed))
        {
            app_motorControl_snapshot_S snapshot = { 0 };
            if ((app_motorControl_getSnapshot(data->config->motor, &snapshot)) &&
                (snapshot.state == APP_MOTORCONTROL_STATE_FAULTED))
            {
                app_motorControl_clearFault(data->config->motor);
            }
            data->gestureConsumed = true;
        }

        // A tap is a release the long-hold gesture did not consume. A second
        // tap within the window cycles the ring's display mode; a lone tap
        // toggles run/stop once the window elapses with no second tap.
        const bool isTap = isButtonReleasedEdge && (!data->gestureConsumed);
        bool isToggle = false;

        if (isTap)
        {
            if (data->tapPending)
            {
                app_rgbLedRing_cycleMode(data->config->ring);
                data->tapPending = false;
                lib_timer_stopTimer(&data->tapWindow);
            }
            else
            {
                data->tapPending = true;
                lib_timer_startTimer(&data->tapWindow);
            }
        }

        if ((data->tapPending) &&
            (lib_timer_updateTimerAndGetState(&data->tapWindow) == LIB_TIMER_STATE_EXPIRED))
        {
            isToggle = true;
            data->tapPending = false;
            lib_timer_stopTimer(&data->tapWindow);
        }

        switch (data->mode)
        {
            default:
            case APP_USERCONTROLS_MODE_OFF:
                if (isToggle)
                {
                    // entry to VELOCITY mode: command always starts at zero
                    // [impl->fw~mc_008~1]
                    data->mode = APP_USERCONTROLS_MODE_VELOCITY;

                    // TODO - trigger rgb ring animation

                    data->dialPrevRaw_deg = data->dialAngleRaw_deg;
                    data->dialAccum_deg   = 0.0f;
                    data->dialCommand     = 0.0f;
                }
                data->velocityRequest_radPerSec = 0.0f;
                break;

            case APP_USERCONTROLS_MODE_VELOCITY:
                if (isToggle)
                {
                    data->mode = APP_USERCONTROLS_MODE_OFF;

                    // TODO - trigger rgb ring animation
                }

                // compute velocity command from dial encoder angle: sticky
                // signed accumulator, normalized to [-1, +1]
                data->dialCommand = app_userControls_private_dialAccumulate(&data->dialAccum_deg,
                                                                            &data->dialPrevRaw_deg,
                                                                            data->dialAngleRaw_deg);

                // scale the [-1, 1] dial command to the range of motor speed
                data->velocityRequest_radPerSec = data->dialCommand * APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC;
                break;
        }

        // set outputs — only on change, so the controls and app commands
        // share the request state by order of arrival (fw~conn_server_002).
        const app_motorControl_mode_E modeOut =
            ((data->mode == APP_USERCONTROLS_MODE_VELOCITY)) ? APP_MOTORCONTROL_MODE_SIX_STEP_TRAP
                                                             : APP_MOTORCONTROL_MODE_OFF;
        if ((!data->outputsSeeded) || (modeOut != data->modeSent))
        {
            app_motorControl_setMode(data->config->motor, modeOut);
            data->modeSent = modeOut;
        }
        if ((!data->outputsSeeded) ||
            (fabsf(data->velocityRequest_radPerSec - data->velocitySent_radPerSec) >
             APP_USERCONTROLS_VELOCITY_SEND_DEADBAND_RAD_PER_SEC))
        {
            app_motorControl_setVelocity(data->config->motor, data->velocityRequest_radPerSec);
            data->velocitySent_radPerSec = data->velocityRequest_radPerSec;
        }
        data->outputsSeeded = true;
    }
}

