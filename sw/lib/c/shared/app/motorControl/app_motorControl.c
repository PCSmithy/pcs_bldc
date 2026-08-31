/* Includes */
#include <math.h>

#include "lib_utils.h"
#include "lib_timer.h"
#include "lib_filterIIR.h"

#include "app_motorControl.h"


/* Defines */
#define TWO_PI (2.0f * PI)
#define ZERO (0.0f)

#define RAD_30DEG DEG_TO_RAD(30.0f)
#define RAD_60DEG DEG_TO_RAD(60.0f)
#define RAD_90DEG DEG_TO_RAD(90.0f)
#define RAD_120DEG DEG_TO_RAD(120.0f)
#define RAD_180DEG DEG_TO_RAD(180.0f)
#define RAD_240DEG DEG_TO_RAD(240.0f)
#define RAD_300DEG DEG_TO_RAD(300.0f)


#define ALIGNMENT_DWELL_TIMER_MS (500U)
#define ALIGNMENT_DUTY_CYCLE (0.1f)

#define APP_MOTORCONTROL_MAX_DUTY_01 (0.9f)

// In-module overcurrent trip (fw~safety_001). Each control cycle any phase
// current magnitude above OVERCURRENT_PHASE_TRIP_A, or a DC-bus current above
// OVERCURRENT_BUS_TRIP_A, latches the fault; the latch holds the bridge
// disabled until the fault-clear action (fw~mc_007).
#define OVERCURRENT_PHASE_TRIP_A (2.0f)
#define OVERCURRENT_BUS_TRIP_A   (1.5f)

// Encoder-fault trip (fw~safety_002). Commutation depends on rotor position, so
// more than this many CONSECUTIVE invalid encoder reads latches the fault; a
// valid read resets the count.
#define ENCODER_FAULT_LIMIT (5U)

// Velocity estimate (fw~est_velocity_001): first-order IIR on the per-tick
// angle derivative; alpha = dt/tau = 1 ms / 10 ms.
#define VELOCITY_TICK_S      (0.001f)
#define VELOCITY_FILTER_ALPHA (0.1f)

/* Typedefs */

typedef struct
{
    app_motorControl_mode_E modeCurrent;
    app_motorControl_mode_E modeRequested;

    float32_t velocitySetpointCurrent_radPerSec;
    float32_t velocitySetpointRequested_radPerSec;

    float32_t mechanicalAngle_rad;
    float32_t magneticAngle_rad;
    float32_t magneticAngleTarget_rad;

    float32_t velocityMeasured_radPerSec;
    float32_t prevRotorPosition_rad;   // RAW encoder angle: immune to the alignment-offset capture step
    bool velocitySeeded;   // first tick has no previous angle to difference
    lib_filterIIR_channel_S velocityFilter;

    bool isAligned;
    float32_t alignmentOffset_rad;
    lib_timer_channel_S alignmentDwell;

    bool faultLatched;
    bool bridgeEnabled;   // last cycle's bridge output-enable, for the state view
    uint16_t encoderFaultCount;   // consecutive invalid encoder reads

    float32_t duty[IO_BRIDGE_PHASE_COUNT];
    float32_t phaseCurrent_a[IO_BRIDGE_PHASE_COUNT];
    float32_t busCurrent;
    bool enable[IO_BRIDGE_PHASE_COUNT];
} app_motorControl_channelData_S;

typedef struct
{
    const app_motorControl_config_S * config;
    app_motorControl_channelData_S channels[APP_MOTORCONTROL_CHANNEL_COUNT];
} app_motorControl_data_S;

/* Private Function Declarations */
static void app_motorControl_private_updateOvercurrentLatch(
    const app_motorControl_channelConfig_S * const channelConfig,
    app_motorControl_channelData_S * const channelData);

/* Private Data Definitions */
static app_motorControl_data_S app_motorControl_data;
static app_motorControl_data_S * const data = &app_motorControl_data;

/* Private Function Definitions */

// [impl->fw~safety_001~1]
// Latch an overcurrent fault on any phase-current magnitude above the phase
// trip or a DC-bus current above the bus trip.
static void app_motorControl_private_updateOvercurrentLatch(
    const app_motorControl_channelConfig_S * const channelConfig,
    app_motorControl_channelData_S * const channelData)
{
    float32_t amps = ZERO;

    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (IO_bridge_getPhaseCurrent(channelConfig->bridge, (IO_bridge_phase_E)phase, &amps))
        {
            channelData->phaseCurrent_a[phase] = amps;
            if (fabsf(amps) > OVERCURRENT_PHASE_TRIP_A)
            {
                channelData->faultLatched = true;
            }
        }
    }

    if (IO_bridge_getBusCurrent(channelConfig->bridge, &amps))
    {
        channelData->busCurrent = amps;
        if (fabsf(amps) > OVERCURRENT_BUS_TRIP_A)
        {
            channelData->faultLatched = true;
        }
    }
}


/* Public Function Definitions */

bool app_motorControl_init(const app_motorControl_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= APP_MOTORCONTROL_CHANNEL_COUNT))
    {
        bool channelsValid = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            const app_motorControl_channelConfig_S * const cfg = &config->channels[channel];
            channelsValid &= (cfg->gateDriver < DEV_GATEDRIVER_CHANNEL_COUNT);
            channelsValid &= (cfg->bridge < IO_BRIDGE_CHANNEL_COUNT);
            // Both are used as divisors / a commutation multiplier below; a zero
            // would give a degenerate drive (or a division by zero), so reject it
            // at config time rather than defend at every use.
            channelsValid &= (cfg->motorPolePairs > 0U);
            channelsValid &= (cfg->maxVelocity_radPerSec > ZERO);

            lib_filterIIR_channel_S filter =
            {
                .type = LIB_FILTERIIR_TYPE_EMA,
                .ema.alpha = (VELOCITY_TICK_S / cfg->velocityEstimateFilterTau_s),
            };
            channelsValid &= lib_filterIIR_init(&filter);
        }

        if (channelsValid)
        {
            data->config = config;

            for (size_t channel = 0U; channel < config->numChannels; channel++)
            {
                data->channels[channel].modeCurrent = APP_MOTORCONTROL_MODE_OFF;
                data->channels[channel].modeRequested = APP_MOTORCONTROL_MODE_OFF;

                data->channels[channel].isAligned = false;
                data->channels[channel].alignmentOffset_rad = ZERO;
                lib_timer_init(&data->channels[channel].alignmentDwell, LIB_TIMER_PRECISION_MS, ALIGNMENT_DWELL_TIMER_MS);

                data->channels[channel].faultLatched = false;
                data->channels[channel].bridgeEnabled = false;
                data->channels[channel].encoderFaultCount = 0U;

                data->channels[channel].velocitySeeded = false;
                data->channels[channel].velocityMeasured_radPerSec = ZERO;
                data->channels[channel].prevRotorPosition_rad = ZERO;

                data->channels[channel].velocityFilter.type = LIB_FILTERIIR_TYPE_EMA;
                data->channels[channel].velocityFilter.ema.alpha = (VELOCITY_TICK_S / config->channels[channel].velocityEstimateFilterTau_s);
                data->channels[channel].velocityFilter.init = false;
            }
            success = true;
        }
    }
    return success;
}

#define SET_DUTY_AND_ENABLE(dU, dV, dW, enU, enV, enW) \
    channelData->duty[IO_BRIDGE_PHASE_U] = dU; \
    channelData->duty[IO_BRIDGE_PHASE_V] = dV; \
    channelData->duty[IO_BRIDGE_PHASE_W] = dW; \
    channelData->enable[IO_BRIDGE_PHASE_U] = enU; \
    channelData->enable[IO_BRIDGE_PHASE_V] = enV; \
    channelData->enable[IO_BRIDGE_PHASE_W] = enW;

void app_motorControl_run1ms(void)
{
    if (data->config != NULL)
    {

        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            const app_motorControl_channelConfig_S * const channelConfig = &data->config->channels[channel];
            app_motorControl_channelData_S * const channelData = &data->channels[channel];

            // fetch inputs. On a dropped encoder read, hold the last rotor
            // position: readAngle leaves the output untouched on failure, so a
            // pre-seed avoids both an uninitialized read and a commutation jump.
            float32_t rotorPosition_rad = channelData->mechanicalAngle_rad + channelData->alignmentOffset_rad;
            (void)IO_AS5048_readAngle(channelConfig->encoder, NULL, NULL, &rotorPosition_rad);
            channelData->mechanicalAngle_rad = rotorPosition_rad - channelData->alignmentOffset_rad;

            const float32_t magneticOffset = fmodf((channelData->mechanicalAngle_rad * channelConfig->motorPolePairs), TWO_PI);
            channelData->magneticAngle_rad = magneticOffset < ZERO ? (magneticOffset + TWO_PI) : magneticOffset;

            // [impl->fw~est_velocity_001~1] Wrapped per-tick angle derivative,
            // EMA-filtered; the first tick only seeds the previous angle.
            if (channelData->velocitySeeded)
            {
                // Difference the RAW encoder angle: the alignment-offset
                // capture steps mechanicalAngle by up to pi in one tick, which
                // would alias into a huge one-tick velocity spike.
                float32_t delta_rad = rotorPosition_rad - channelData->prevRotorPosition_rad;
                delta_rad = WRAP_RAD_TO_PI(delta_rad);
                const float32_t velocityRaw_radPerSec = delta_rad / VELOCITY_TICK_S;

                channelData->velocityFilter.ema.x_k = velocityRaw_radPerSec;
                if (channelData->velocityFilter.init)
                {
                    lib_filterIIR_update(&channelData->velocityFilter);
                }
                else
                {
                    // First real sample: init seeds the filter output to it.
                    (void)lib_filterIIR_init(&channelData->velocityFilter);
                }
                channelData->velocityMeasured_radPerSec = channelData->velocityFilter.ema.y_k;
            }
            else
            {
                channelData->velocitySeeded = true;
            }

            channelData->prevRotorPosition_rad = rotorPosition_rad;

            // [impl->fw~safety_002~1] Latch a fault on a persistently invalid
            // encoder — a stale position would commutate the live bridge wrong.
            IO_AS5048_status_E encoderStatus = IO_AS5048_STATUS_IDLE;
            (void)IO_AS5048_getStatus(channelConfig->encoder, &encoderStatus);
            if (encoderStatus == IO_AS5048_STATUS_FAULT)
            {
                channelData->encoderFaultCount++;
                if (channelData->encoderFaultCount > ENCODER_FAULT_LIMIT)
                {
                    channelData->faultLatched = true;
                }
            }
            else
            {
                channelData->encoderFaultCount = 0U;
            }

            app_motorControl_private_updateOvercurrentLatch(channelConfig, channelData);

            // Apply any requested mode transition up front, so a request (e.g.
            // OFF) takes effect this cycle instead of driving one cycle late.
            channelData->modeCurrent = channelData->modeRequested;

            // [impl->fw~mc_006~1]
            const bool driveAllowed =
                (dev_gateDriver_isOperational(channelConfig->gateDriver)) &&
                (!channelData->faultLatched);

            // compute state
            bool isAnyPhaseEnabled = false;

            if (!driveAllowed)
            {
                channelData->velocitySetpointCurrent_radPerSec = ZERO;
                SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, false, false, false);
                lib_timer_stopTimer(&channelData->alignmentDwell);
            }
            else
            {
                switch (channelData->modeCurrent)
                {
                    default:
                    case APP_MOTORCONTROL_MODE_OFF:
                        channelData->velocitySetpointCurrent_radPerSec = ZERO;
                        SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, false, false, false);
                        lib_timer_stopTimer(&channelData->alignmentDwell);
                        break;

                    case APP_MOTORCONTROL_MODE_SIX_STEP_TRAP:
                        // [impl->fw~mc_012~1]
                        // First enable: drive the alignment pattern for the dwell,
                        // then capture the shaft angle as the offset.
                        if (!channelData->isAligned)
                        {
                            SET_DUTY_AND_ENABLE(ALIGNMENT_DUTY_CYCLE, ZERO, ZERO, true, true, false);
                            isAnyPhaseEnabled = true;

                            if (lib_timer_runTimerWithEnable(&channelData->alignmentDwell, true) == LIB_TIMER_STATE_EXPIRED)
                            {
                                channelData->alignmentOffset_rad = rotorPosition_rad;
                                channelData->isAligned = true;
                            }
                        }
                        // [impl->fw~mc_011~1]
                        // Commutate: apply the sector pattern for the rotor
                        // electrical angle advanced by the lead, at a duty
                        // proportional to the target magnitude, clamped to max.
                        else
                        {
                            channelData->velocitySetpointCurrent_radPerSec = channelData->velocitySetpointRequested_radPerSec;


                            const float32_t lead_rad = (channelData->velocitySetpointCurrent_radPerSec >= ZERO) ? RAD_90DEG : -RAD_90DEG;
                            float32_t target = fmodf((channelData->magneticAngle_rad + lead_rad), TWO_PI);
                            if (target < ZERO) { target += TWO_PI; }
                            channelData->magneticAngleTarget_rad = target;

                            // TODO - compute duty cycle from velocity error
                            const float32_t duty01 = MIN_OF(fabsf(channelData->velocitySetpointCurrent_radPerSec) / channelConfig->maxVelocity_radPerSec, APP_MOTORCONTROL_MAX_DUTY_01);

                            // Each branch applies the field at its bucket's LOWER edge
                            // (0/60/../300 deg), so an unbiased lookup under-rotates the
                            // field by 0..60 deg. Biasing by +30 deg rounds the target
                            // to the NEAREST producible field angle instead, keeping the
                            // effective lead at 60..120 deg for either rotation sign.
                            const float32_t sector_rad = fmodf((target + RAD_30DEG), TWO_PI);
                            if (sector_rad < RAD_60DEG)
                            {
                                SET_DUTY_AND_ENABLE(duty01, ZERO, ZERO, true, true, false);
                            }
                            else if (sector_rad < RAD_120DEG)
                            {
                                SET_DUTY_AND_ENABLE(duty01, ZERO, ZERO, true, false, true);
                            }
                            else if (sector_rad < RAD_180DEG)
                            {
                                SET_DUTY_AND_ENABLE(ZERO, duty01, ZERO, false, true, true);
                            }
                            else if (sector_rad < RAD_240DEG)
                            {
                                SET_DUTY_AND_ENABLE(ZERO, duty01, ZERO, true, true, false);
                            }
                            else if (sector_rad < RAD_300DEG)
                            {
                                SET_DUTY_AND_ENABLE(ZERO, ZERO, duty01, true, false, true);
                            }
                            else // angle < 360
                            {
                                SET_DUTY_AND_ENABLE(ZERO, ZERO, duty01, false, true, true);
                            }
                            isAnyPhaseEnabled = true;
                        }
                        break;
                }
            }

            // update outputs
            IO_bridge_setPhaseOutputEnabled(channelConfig->bridge, IO_BRIDGE_PHASE_U, channelData->enable[IO_BRIDGE_PHASE_U]);
            IO_bridge_setPhaseOutputEnabled(channelConfig->bridge, IO_BRIDGE_PHASE_V, channelData->enable[IO_BRIDGE_PHASE_V]);
            IO_bridge_setPhaseOutputEnabled(channelConfig->bridge, IO_BRIDGE_PHASE_W, channelData->enable[IO_BRIDGE_PHASE_W]);

            IO_bridge_setPhaseDuty(channelConfig->bridge, IO_BRIDGE_PHASE_U, channelData->duty[IO_BRIDGE_PHASE_U]);
            IO_bridge_setPhaseDuty(channelConfig->bridge, IO_BRIDGE_PHASE_V, channelData->duty[IO_BRIDGE_PHASE_V]);
            IO_bridge_setPhaseDuty(channelConfig->bridge, IO_BRIDGE_PHASE_W, channelData->duty[IO_BRIDGE_PHASE_W]);

            IO_bridge_setOutputEnabled(channelConfig->bridge, isAnyPhaseEnabled);

            channelData->bridgeEnabled = isAnyPhaseEnabled;
        }
    }
}

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec)
{
    if ((data->config != NULL) && (channel < APP_MOTORCONTROL_CHANNEL_COUNT))
    {
        const float32_t velocitySaturated_radPerSec = MIN_OF(fabsf(velocity_radPerSec), data->config->channels[channel].maxVelocity_radPerSec);
        data->channels[channel].velocitySetpointRequested_radPerSec = SIGN(velocity_radPerSec) * velocitySaturated_radPerSec;
    }
}

void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode)
{
    if ((data->config != NULL) &&
        (channel < APP_MOTORCONTROL_CHANNEL_COUNT) &&
        (mode < APP_MOTORCONTROL_MODE_COUNT))
    {
        data->channels[channel].modeRequested = mode;
    }
}

// [impl->fw~safety_001~1]
void app_motorControl_clearFault(app_motorControl_channel_E channel)
{
    if ((data->config != NULL) && (channel < APP_MOTORCONTROL_CHANNEL_COUNT))
    {
        data->channels[channel].faultLatched = false;
        data->channels[channel].encoderFaultCount = 0U;
    }
}

bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (channel < APP_MOTORCONTROL_CHANNEL_COUNT) &&
        (snapshot != NULL))
    {
        const app_motorControl_channelData_S * const channelData = &data->channels[channel];
        snapshot->mode              = channelData->modeCurrent;
        snapshot->isAligned         = channelData->isAligned;
        snapshot->magneticAngle_rad = channelData->magneticAngle_rad;
        snapshot->velocitySetpoint_radPerSec = channelData->velocitySetpointCurrent_radPerSec;
        snapshot->velocityMeasured_radPerSec = channelData->velocityMeasured_radPerSec;

        // Coarse state the ring reads for fw~mc_009 (the ring carries that impl
        // tag): fault wins, else the live bridge-enable distinguishes driving
        // from idle.
        if (channelData->faultLatched)
        {
            snapshot->state = APP_MOTORCONTROL_STATE_FAULTED;
        }
        else
        {
            snapshot->state = channelData->bridgeEnabled
                                  ? APP_MOTORCONTROL_STATE_ENABLED
                                  : APP_MOTORCONTROL_STATE_DISABLED;
        }
        ret = true;
    }

    return ret;
}

