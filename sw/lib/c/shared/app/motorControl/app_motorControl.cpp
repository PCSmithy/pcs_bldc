/* Includes */
#include <math.h>

#include "lib_utils.h"
#include "lib_timer.h"
#include "lib_filterIIR.h"

#include "app_motorControl.h"

#include <array>


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

class Motor
{
    public:
        bool init(const app_motorControl_channelConfig_S * config);
        void run1ms();

        bool getSnapshot(app_motorControl_snapshot_S * const snapshot); // Q: could this be a Friend function?
        void setVelocity(float32_t velocity_radPerSec);
        void setMode(app_motorControl_mode_E mode);
        void clearFault();

    private:
        void updateOvercurrentLatch();

        app_motorControl_channelConfig_S const * config;

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
};


typedef struct
{
    std::array<Motor, APP_MOTORCONTROL_CHANNEL_COUNT> channels;
} app_motorControl_data_S;

/* Private Free Function Declarations */


/* Private Data Definitions */
constinit static app_motorControl_data_S app_motorControl_data = {};
constinit static app_motorControl_data_S * const data = &app_motorControl_data;

/* Private Free Function Definitions */


/* Private Class Function Definitions */

// [impl->fw~safety_001~1]
// Latch an overcurrent fault on any phase-current magnitude above the phase
// trip or a DC-bus current above the bus trip.
void Motor::updateOvercurrentLatch()
{
    float32_t amps = ZERO;

    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (IO_bridge_getPhaseCurrent(this->config->bridge, (IO_bridge_phase_E)phase, &amps))
        {
            this->phaseCurrent_a[phase] = amps;
            if (fabsf(amps) > OVERCURRENT_PHASE_TRIP_A)
            {
                this->faultLatched = true;
            }
        }
    }

    if (IO_bridge_getBusCurrent(this->config->bridge, &amps))
    {
        this->busCurrent = amps;
        if (fabsf(amps) > OVERCURRENT_BUS_TRIP_A)
        {
            this->faultLatched = true;
        }
    }
}

/* Public Class Function Definitions */
bool Motor::init(const app_motorControl_channelConfig_S * channelConfig)
{
    bool isValid = true;

    isValid &= (channelConfig->gateDriver < DEV_GATEDRIVER_CHANNEL_COUNT);
    isValid &= (channelConfig->bridge < IO_BRIDGE_CHANNEL_COUNT);
    // Both are used as divisors / a commutation multiplier below; a zero
    // would give a degenerate drive (or a division by zero), so reject it
    // at config time rather than defend at every use.
    isValid &= (channelConfig->motorPolePairs > 0U);
    isValid &= (channelConfig->maxVelocity_radPerSec > ZERO);

    lib_filterIIR_channel_S filter =
    {
        .type = LIB_FILTERIIR_TYPE_EMA,
        .ema = {
            .alpha = (VELOCITY_TICK_S / channelConfig->velocityEstimateFilterTau_s),
            .y_k = 0.0f,
            .y_k_1 = 0.0f,
            .x_k = 0.0f,
        },
        .init = false,
    };
    isValid &= lib_filterIIR_init(&filter);

    if (isValid)
    {
        this->config = channelConfig;

        this->modeCurrent = APP_MOTORCONTROL_MODE_OFF;
        this->modeRequested = APP_MOTORCONTROL_MODE_OFF;

        this->isAligned = false;
        this->alignmentOffset_rad = ZERO;
        lib_timer_init(&this->alignmentDwell, LIB_TIMER_PRECISION_MS, ALIGNMENT_DWELL_TIMER_MS);

        this->faultLatched = false;
        this->bridgeEnabled = false;
        this->encoderFaultCount = 0U;

        this->velocitySeeded = false;
        this->velocityMeasured_radPerSec = ZERO;
        this->prevRotorPosition_rad = ZERO;

        this->velocityFilter.type = LIB_FILTERIIR_TYPE_EMA;
        this->velocityFilter.ema.alpha = (VELOCITY_TICK_S / channelConfig->velocityEstimateFilterTau_s);
        this->velocityFilter.init = false;
    }
    return isValid;
}

#define SET_DUTY_AND_ENABLE(dU, dV, dW, enU, enV, enW) \
    this->duty[IO_BRIDGE_PHASE_U] = dU; \
    this->duty[IO_BRIDGE_PHASE_V] = dV; \
    this->duty[IO_BRIDGE_PHASE_W] = dW; \
    this->enable[IO_BRIDGE_PHASE_U] = enU; \
    this->enable[IO_BRIDGE_PHASE_V] = enV; \
    this->enable[IO_BRIDGE_PHASE_W] = enW;


void Motor::run1ms()
{
    if (this->config != NULL)
    {
        // fetch inputs. On a dropped encoder read, hold the last rotor
        // position: readAngle leaves the output untouched on failure, so a
        // pre-seed avoids both an uninitialized read and a commutation jump.
        float32_t rotorPosition_rad = this->mechanicalAngle_rad + this->alignmentOffset_rad;
        (void)IO_AS5048_readAngle(this->config->encoder, NULL, NULL, &rotorPosition_rad);
        this->mechanicalAngle_rad = rotorPosition_rad - this->alignmentOffset_rad;

        const float32_t magneticOffset = fmodf((this->mechanicalAngle_rad * this->config->motorPolePairs), TWO_PI);
        this->magneticAngle_rad = magneticOffset < ZERO ? (magneticOffset + TWO_PI) : magneticOffset;

        // [impl->fw~est_velocity_001~1] Wrapped per-tick angle derivative,
        // EMA-filtered; the first tick only seeds the previous angle.
        if (this->velocitySeeded)
        {
            // Difference the RAW encoder angle: the alignment-offset
            // capture steps mechanicalAngle by up to pi in one tick, which
            // would alias into a huge one-tick velocity spike.
            float32_t delta_rad = rotorPosition_rad - this->prevRotorPosition_rad;
            delta_rad = WRAP_RAD_TO_PI(delta_rad);
            const float32_t velocityRaw_radPerSec = delta_rad / VELOCITY_TICK_S;

            this->velocityFilter.ema.x_k = velocityRaw_radPerSec;
            if (this->velocityFilter.init)
            {
                lib_filterIIR_update(&this->velocityFilter);
            }
            else
            {
                // First real sample: init seeds the filter output to it.
                (void)lib_filterIIR_init(&this->velocityFilter);
            }
            this->velocityMeasured_radPerSec = this->velocityFilter.ema.y_k;
        }
        else
        {
            this->velocitySeeded = true;
        }

        this->prevRotorPosition_rad = rotorPosition_rad;

        // [impl->fw~safety_002~1] Latch a fault on a persistently invalid
        // encoder — a stale position would commutate the live bridge wrong.
        IO_AS5048_status_E encoderStatus = IO_AS5048_STATUS_IDLE;
        (void)IO_AS5048_getStatus(this->config->encoder, &encoderStatus);
        if (encoderStatus == IO_AS5048_STATUS_FAULT)
        {
            this->encoderFaultCount++;
            if (this->encoderFaultCount > ENCODER_FAULT_LIMIT)
            {
                this->faultLatched = true;
            }
        }
        else
        {
            this->encoderFaultCount = 0U;
        }

        this->updateOvercurrentLatch();

        // Apply any requested mode transition up front, so a request (e.g.
        // OFF) takes effect this cycle instead of driving one cycle late.
        this->modeCurrent = this->modeRequested;

        // [impl->fw~mc_006~1]
        const bool driveAllowed =
            (dev_gateDriver_isOperational(this->config->gateDriver)) &&
            (!this->faultLatched);

        // compute state
        bool isAnyPhaseEnabled = false;

        if (!driveAllowed)
        {
            this->velocitySetpointCurrent_radPerSec = ZERO;
            SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, false, false, false);
            lib_timer_stopTimer(&this->alignmentDwell);
        }
        else
        {
            switch (this->modeCurrent)
            {
                default:
                case APP_MOTORCONTROL_MODE_OFF:
                    this->velocitySetpointCurrent_radPerSec = ZERO;
                    SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, false, false, false);
                    lib_timer_stopTimer(&this->alignmentDwell);
                    break;

                case APP_MOTORCONTROL_MODE_SIX_STEP_TRAP:
                    // [impl->fw~mc_012~1]
                    // First enable: drive the alignment pattern for the dwell,
                    // then capture the shaft angle as the offset.
                    if (!this->isAligned)
                    {
                        SET_DUTY_AND_ENABLE(ALIGNMENT_DUTY_CYCLE, ZERO, ZERO, true, true, false);
                        isAnyPhaseEnabled = true;

                        if (lib_timer_runTimerWithEnable(&this->alignmentDwell, true) == LIB_TIMER_STATE_EXPIRED)
                        {
                            this->alignmentOffset_rad = rotorPosition_rad;
                            this->isAligned = true;
                        }
                    }
                    // [impl->fw~mc_011~1]
                    // Commutate: apply the sector pattern for the rotor
                    // electrical angle advanced by the lead, at a duty
                    // proportional to the target magnitude, clamped to max.
                    else
                    {
                        this->velocitySetpointCurrent_radPerSec = this->velocitySetpointRequested_radPerSec;


                        const float32_t lead_rad = (this->velocitySetpointCurrent_radPerSec >= ZERO) ? RAD_90DEG : -RAD_90DEG;
                        float32_t target = fmodf((this->magneticAngle_rad + lead_rad), TWO_PI);
                        if (target < ZERO) { target += TWO_PI; }
                        this->magneticAngleTarget_rad = target;

                        // TODO - compute duty cycle from velocity error
                        const float32_t duty01 = MIN_OF(fabsf(this->velocitySetpointCurrent_radPerSec) / this->config->maxVelocity_radPerSec, APP_MOTORCONTROL_MAX_DUTY_01);

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
        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_U, this->enable[IO_BRIDGE_PHASE_U]);
        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_V, this->enable[IO_BRIDGE_PHASE_V]);
        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_W, this->enable[IO_BRIDGE_PHASE_W]);

        IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_U, this->duty[IO_BRIDGE_PHASE_U]);
        IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_V, this->duty[IO_BRIDGE_PHASE_V]);
        IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_W, this->duty[IO_BRIDGE_PHASE_W]);

        IO_bridge_setOutputEnabled(this->config->bridge, isAnyPhaseEnabled);

        this->bridgeEnabled = isAnyPhaseEnabled;

    }

}

bool Motor::getSnapshot(app_motorControl_snapshot_S * const snapshot)
{
    bool ret = false;
    if ((this->config != NULL) && (snapshot != NULL))
    {
        snapshot->mode              = this->modeCurrent;
        snapshot->isAligned         = this->isAligned;
        snapshot->magneticAngle_rad = this->magneticAngle_rad;
        snapshot->velocitySetpoint_radPerSec = this->velocitySetpointCurrent_radPerSec;
        snapshot->velocityMeasured_radPerSec = this->velocityMeasured_radPerSec;

        // Coarse state the ring reads for fw~mc_009 (the ring carries that impl
        // tag): fault wins, else the live bridge-enable distinguishes driving
        // from idle.
        if (this->faultLatched)
        {
            snapshot->state = APP_MOTORCONTROL_STATE_FAULTED;
        }
        else
        {
            snapshot->state = this->bridgeEnabled
                                  ? APP_MOTORCONTROL_STATE_ENABLED
                                  : APP_MOTORCONTROL_STATE_DISABLED;
        }
        ret = true;
    }
    return ret;
}

void Motor::setVelocity(float32_t velocity_radPerSec)
{
    if (this->config != NULL)
    {
        const float32_t velocitySaturated_radPerSec = MIN_OF(fabsf(velocity_radPerSec), this->config->maxVelocity_radPerSec);
        this->velocitySetpointRequested_radPerSec = SIGN(velocity_radPerSec) * velocitySaturated_radPerSec;
    }
}

void Motor::setMode(app_motorControl_mode_E mode)
{
    if ((this->config != NULL) && (mode < APP_MOTORCONTROL_MODE_COUNT))
    {
        this->modeRequested = mode;
    }
}

void Motor::clearFault()
{
    if (this->config != NULL)
    {
        this->faultLatched = false;
        this->encoderFaultCount = 0U;
    }
}


/* Public Function Definitions */

bool app_motorControl_init(const app_motorControl_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= APP_MOTORCONTROL_CHANNEL_COUNT))
    {
        // config validation
        bool configsValid = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            configsValid &= data->channels[channel].init(&config->channels[channel]);
        }
        success = configsValid;
    }
    return success;
}

void app_motorControl_run1ms(void)
{
    for (size_t channel = 0U; channel < APP_MOTORCONTROL_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run1ms();
    }
}

bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot)
{
    bool ret = false;

    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        ret = data->channels[channel].getSnapshot(snapshot);
    }

    return ret;
}

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].setVelocity(velocity_radPerSec);
    }
}

void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].setMode(mode);
    }
}

// [impl->fw~safety_001~1]
void app_motorControl_clearFault(app_motorControl_channel_E channel)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].clearFault();
    }
}
