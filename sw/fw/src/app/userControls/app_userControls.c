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
// against a host command (sys~ops_002 arbitration).
#define APP_USERCONTROLS_VELOCITY_SEND_DEADBAND_RAD_PER_SEC (0.05f)
// The same for a position write under the position method: 0.01 rad is about
// half a degree of dial travel, clear of the dial encoder's noise, so a host
// position sequence is not rewritten by a dial that only jitters.
#define APP_USERCONTROLS_POSITION_SEND_DEADBAND_RAD (0.01f)

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
    float32_t positionRequest_rad;

    // The dial's targets as last written, and the method they were written
    // under: only dial movement writes, so a host's targets stand (sys~ops_002).
    float32_t velocitySent_radPerSec;
    float32_t positionSent_rad;
    app_motorControl_mode_E selectedModeSeen;
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
            *data = (app_userControls_data_S){ 0 };
            data->config = config;
            data->mode = APP_USERCONTROLS_MODE_OFF;
            lib_timer_init(&data->holdTimer, LIB_TIMER_PRECISION_MS, APP_USERCONTROLS_FAULT_CLEAR_HOLD_MS);
            lib_timer_init(&data->tapWindow, LIB_TIMER_PRECISION_MS, APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS);
            data->selectedModeSeen = APP_MOTORCONTROL_MODE_COUNT;   // the first pass seeds, writing nothing
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
        app_motorControl_snapshot_S motor = { 0 };
        (void)app_motorControl_getSnapshot(data->config->motor, &motor);

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
            if (motor.state == APP_MOTORCONTROL_STATE_FAULTED)
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

        // A tap stops a drive that is enabled or requested, whoever started it
        // (sys~ops_002), and otherwise starts the selected method.
        if (isToggle)
        {
            const bool isRunning = (motor.state != APP_MOTORCONTROL_STATE_DISABLED) ||
                                   (motor.mode != APP_MOTORCONTROL_MODE_OFF);
            if (isRunning)
            {
                data->mode = APP_USERCONTROLS_MODE_OFF;
                app_motorControl_setMode(data->config->motor, APP_MOTORCONTROL_MODE_OFF);
            }
            else
            {
                // [impl->fw~mc_008~1] the dial command starts at zero
                data->mode = APP_USERCONTROLS_MODE_VELOCITY;
                data->dialPrevRaw_deg = data->dialAngleRaw_deg;
                data->dialAccum_deg   = 0.0f;
                app_motorControl_setMode(data->config->motor, motor.selectedMode);
            }
        }

        // The dial moves its targets only while the controls run the motor: a
        // stop leaves them where they stand.
        if (data->mode == APP_USERCONTROLS_MODE_VELOCITY)
        {
            data->dialCommand = app_userControls_private_dialAccumulate(&data->dialAccum_deg,
                                                                        &data->dialPrevRaw_deg,
                                                                        data->dialAngleRaw_deg);
            data->velocityRequest_radPerSec = data->dialCommand * APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC;
            data->positionRequest_rad = DEG_TO_RAD(data->dialAccum_deg);   // half a turn each way
        }

        // Write a target only when the dial moved past its deadband under an
        // unchanged method; a method change re-seeds without writing.
        if (motor.selectedMode != data->selectedModeSeen)
        {
            data->selectedModeSeen = motor.selectedMode;
            data->velocitySent_radPerSec = data->velocityRequest_radPerSec;
            data->positionSent_rad = data->positionRequest_rad;
        }
        else if (motor.selectedMode == APP_MOTORCONTROL_MODE_FOC_POSITION)
        {
            if (fabsf(data->positionRequest_rad - data->positionSent_rad) > APP_USERCONTROLS_POSITION_SEND_DEADBAND_RAD)
            {
                app_motorControl_setPositionSetpoint(data->config->motor, data->positionRequest_rad, 0.0f, 0.0f, 0.0f);   // at the maximum velocity, stepping (fw~mc_031)
                data->positionSent_rad = data->positionRequest_rad;
            }
        }
        else if (fabsf(data->velocityRequest_radPerSec - data->velocitySent_radPerSec) >
                 APP_USERCONTROLS_VELOCITY_SEND_DEADBAND_RAD_PER_SEC)
        {
            app_motorControl_setVelocity(data->config->motor, data->velocityRequest_radPerSec, 0.0f);   // the dial steps (fw~mc_027)
            data->velocitySent_radPerSec = data->velocityRequest_radPerSec;
        }
        else
        {
            // the dial holds: the target in effect stands
        }
    }
}

