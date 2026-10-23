/* Includes */
#include <math.h>

#include "lib_utils.h"
#include "lib_timer.h"
#include "lib_transforms.h"
#include "lib_filterIIR.h"
#include "HW_CORDIC.h"

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
        void run50us();

        bool getSnapshot(app_motorControl_snapshot_S * const snapshot);
        void setVelocity(float32_t velocity_radPerSec);
        void setMode(app_motorControl_mode_E mode);
        void clearFault();

    private:
        // What the bridge is doing this millisecond, decided by run1ms from the
        // mode, the gate, and the alignment latch; run50us acts on it.
        enum class DriveState : uint8_t
        {
            Off,          // bridge dark: gate closed, fault latched, or mode OFF
            Precharging,  // every enable: low sides on, charging the bootstraps
            Aligning,     // first enable: the alignment pattern for the dwell
            Driving,      // the active method commutates
        };

        // helper functions
        void updateMagneticAngle();


        // called per PWM cycle
        void fetchPhaseCurrents();
        void updateRotorPosition();

        // called in run1ms()
        void updateOvercurrentLatch();

        app_motorControl_channelConfig_S const * config;

        app_motorControl_mode_E modeCurrent;
        app_motorControl_mode_E modeRequested;
        app_motorControl_mode_E modeSelected;   // the method an enable runs; OFF keeps it
        DriveState driveState;

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
        lib_timer_channel_S prechargeDwell;

        bool isDriveAllowed;
        bool faultLatched;
        bool bridgeEnabled;   // last cycle's bridge output-enable, for the state view
        uint16_t encoderFaultCount;   // consecutive invalid encoder reads

        // setpoint
        float32_t duty[IO_BRIDGE_PHASE_COUNT];
        float32_t vAlpha_v;
        float32_t vBeta_v;

        // measured
        float32_t phaseCurrent_a[IO_BRIDGE_PHASE_COUNT];

        float32_t iAlpha_a;
        float32_t iBeta_a;
        float32_t iD_a;
        float32_t iQ_a;

        float32_t busCurrent;
        float32_t busVoltage;
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

void Motor::updateMagneticAngle()
{
    this->magneticAngle_rad = lib_transforms_wrapAngle(this->mechanicalAngle_rad * this->config->motorPolePairs);
}

void Motor::fetchPhaseCurrents()
{
    float32_t amps = ZERO;

    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (IO_bridge_getPhaseCurrent(this->config->bridge, (IO_bridge_phase_E)phase, &amps))
        {
            this->phaseCurrent_a[phase] = amps;
        }
    }
}

// [impl->fw~mc_019~1]
// [impl->fw~mc_019~1] the 1 ms sample advanced one PWM period per cycle at
// the electrical velocity; run1ms re-anchors it on the next encoder read.
void Motor::updateRotorPosition()
{
    if (this->velocitySeeded)
    {
        this->mechanicalAngle_rad += this->velocityMeasured_radPerSec * this->config->pwmPeriod_s;

        this->updateMagneticAngle();
    }
}

// [impl->fw~safety_001~1]
// Latch an overcurrent fault on any phase-current magnitude above the phase
// trip or a DC-bus current above the bus trip.
void Motor::updateOvercurrentLatch()
{
    float32_t amps = ZERO;

    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (fabsf(this->phaseCurrent_a[phase]) > OVERCURRENT_PHASE_TRIP_A)
        {
            this->faultLatched = true;
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
    isValid &= (channelConfig->pwmPeriod_s > ZERO);
    isValid &= (channelConfig->maxVoltage_V > ZERO);
    isValid &= (channelConfig->defaultMode > APP_MOTORCONTROL_MODE_OFF);
    isValid &= (channelConfig->defaultMode < APP_MOTORCONTROL_MODE_COUNT);
    isValid &= (channelConfig->prechargeTime_ms > 0U);

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
        this->modeSelected = channelConfig->defaultMode;
        this->driveState = DriveState::Off;

        this->isAligned = false;
        this->alignmentOffset_rad = ZERO;
        lib_timer_init(&this->alignmentDwell, LIB_TIMER_PRECISION_MS, ALIGNMENT_DWELL_TIMER_MS);
        lib_timer_init(&this->prechargeDwell, LIB_TIMER_PRECISION_MS, channelConfig->prechargeTime_ms);

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

// Written by run1ms (task) while Off or Aligning, by run50us (ISR) while
// Driving; never both in one state. A cycle that reads a half-written
// alignment pattern outputs it for one PWM period, and the pattern is static.
#define SET_ENABLE(enU, enV, enW) \
    this->enable[IO_BRIDGE_PHASE_U] = enU; \
    this->enable[IO_BRIDGE_PHASE_V] = enV; \
    this->enable[IO_BRIDGE_PHASE_W] = enW;


#define SET_DUTY_AND_ENABLE(dU, dV, dW, enU, enV, enW) \
    this->duty[IO_BRIDGE_PHASE_U] = dU; \
    this->duty[IO_BRIDGE_PHASE_V] = dV; \
    this->duty[IO_BRIDGE_PHASE_W] = dW; \
    SET_ENABLE(enU, enV, enW)


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

        this->updateMagneticAngle();

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

        float32_t busVoltage = 0.0f;
        if (IO_bridge_getBusVoltage(this->config->bridge, &busVoltage))
        {
            this->busVoltage = busVoltage;
        }


        // Apply any requested mode transition up front, so a request (e.g.
        // OFF) takes effect this cycle instead of driving one cycle late.
        this->modeCurrent = this->modeRequested;

        // [impl->fw~mc_006~1]
        this->isDriveAllowed =
            (dev_gateDriver_isOperational(this->config->gateDriver)) &&
            (!this->faultLatched);

        if ((!this->isDriveAllowed) || (this->modeCurrent == APP_MOTORCONTROL_MODE_OFF))
        {
            this->driveState = DriveState::Off;
        }
        else if ((this->driveState == DriveState::Off) || (this->driveState == DriveState::Precharging))
        {
            // [impl->fw~mc_021~1] every enable from dark charges the bootstraps
            // through the low sides before a high side is asked to switch: a
            // dark bridge's bootstraps have drained, and a high side commanded
            // on from an empty one trips the gate driver's VDS protection.
            const bool charged =
                (lib_timer_runTimerWithEnable(&this->prechargeDwell, true) == LIB_TIMER_STATE_EXPIRED);
            this->driveState = charged ? (this->isAligned ? DriveState::Driving : DriveState::Aligning)
                                       : DriveState::Precharging;
        }
        else
        {
            this->driveState = this->isAligned ? DriveState::Driving : DriveState::Aligning;
        }

        switch (this->driveState)
        {
            case DriveState::Off:
                this->velocitySetpointCurrent_radPerSec = ZERO;
                SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, false, false, false);
                lib_timer_stopTimer(&this->alignmentDwell);
                lib_timer_stopTimer(&this->prechargeDwell);
                break;

            case DriveState::Precharging:
                SET_DUTY_AND_ENABLE(ZERO, ZERO, ZERO, true, true, true);
                break;

            case DriveState::Aligning:
                // [impl->fw~mc_012~1]
                // First enable: a stator field along the phase-U axis (U
                // sourcing, V and W returning) parks the rotor flux on U for
                // the dwell; the shaft angle there is electrical zero.
                SET_DUTY_AND_ENABLE(ALIGNMENT_DUTY_CYCLE, ZERO, ZERO, true, true, true);
                if (lib_timer_runTimerWithEnable(&this->alignmentDwell, true) == LIB_TIMER_STATE_EXPIRED)
                {
                    this->alignmentOffset_rad = rotorPosition_rad;
                    this->isAligned = true;
                }
                break;

            case DriveState::Driving:
                // The active method's step owns the outputs (run50us).
                break;
        }
    }
}

// [impl->fw~mc_015~1] the PWM-synchronous step, in the bridge callback's ISR
// context: the active method commutates from the inputs run1ms published and
// the cycle's phase currents, then the bridge takes the outputs.
void Motor::run50us()
{
    if (this->config != NULL)
    {
        this->fetchPhaseCurrents();

        this->updateRotorPosition();

        if (this->driveState == DriveState::Driving)
        {
            this->velocitySetpointCurrent_radPerSec = this->velocitySetpointRequested_radPerSec;
        }

        bool isAnyPhaseEnabled = (this->driveState != DriveState::Off);
        bool isVectorControl = false;

        // One sine/cosine pair of the rotor angle serves every transform this
        // cycle. The current frame rotates with the rotor only while a method
        // drives; otherwise it is the stationary frame.
        float32_t rotorSin = ZERO;
        float32_t rotorCos = 1.0f;
        (void) HW_CORDIC_sinCos(this->magneticAngle_rad, &rotorSin, &rotorCos);
        float32_t frameSin = ZERO;
        float32_t frameCos = 1.0f;
        if (this->driveState == DriveState::Driving)
        {
            switch (this->modeCurrent)
            {
                default:
                case APP_MOTORCONTROL_MODE_OFF:
                    // Off is DriveState::Off, handled in run1ms.
                    break;

                case APP_MOTORCONTROL_MODE_SIX_STEP_TRAP:
                {
                    frameSin = rotorSin;
                    frameCos = rotorCos;

                    // [impl->fw~mc_011~1]
                    // Commutate: apply the sector pattern for the rotor
                    // electrical angle advanced by the lead, at a duty
                    // proportional to the target magnitude, clamped to max.

                    const float32_t lead_rad = (this->velocitySetpointCurrent_radPerSec >= ZERO) ? RAD_90DEG : -RAD_90DEG;
                    const float32_t target = lib_transforms_wrapAngle(this->magneticAngle_rad + lead_rad);
                    this->magneticAngleTarget_rad = target;

                    // TODO - compute duty cycle from velocity error
                    const float32_t duty01 = MIN_OF(fabsf(this->velocitySetpointCurrent_radPerSec) / this->config->maxVelocity_radPerSec, APP_MOTORCONTROL_MAX_DUTY_01);

                    // A two-phase pattern's field lies at 30/90/../330 deg,
                    // the centre of its 60 deg sector, so the sector holding the
                    // target applies the nearest producible field: the effective
                    // lead stays within 30 deg of the commanded one either way.
                    if (target < RAD_60DEG)
                    {
                        SET_DUTY_AND_ENABLE(duty01, ZERO, ZERO, true, false, true);   // U+ W-  (30 deg)
                    }
                    else if (target < RAD_120DEG)
                    {
                        SET_DUTY_AND_ENABLE(ZERO, duty01, ZERO, false, true, true);   // V+ W-  (90 deg)
                    }
                    else if (target < RAD_180DEG)
                    {
                        SET_DUTY_AND_ENABLE(ZERO, duty01, ZERO, true, true, false);   // V+ U- (150 deg)
                    }
                    else if (target < RAD_240DEG)
                    {
                        SET_DUTY_AND_ENABLE(ZERO, ZERO, duty01, true, false, true);   // W+ U- (210 deg)
                    }
                    else if (target < RAD_300DEG)
                    {
                        SET_DUTY_AND_ENABLE(ZERO, ZERO, duty01, false, true, true);   // W+ V- (270 deg)
                    }
                    else // angle < 360
                    {
                        SET_DUTY_AND_ENABLE(duty01, ZERO, ZERO, true, true, false);   // U+ V- (330 deg)
                    }
                    break;
                }

                // [impl->fw~mc_020~1]
                case APP_MOTORCONTROL_MODE_SINUSOIDAL:
                {
                    isVectorControl = true;
                    frameSin = rotorSin;
                    frameCos = rotorCos;

                    // compute rotor frame voltage
                    const float32_t velocityPercent = MIN_OF(fabsf(this->velocitySetpointCurrent_radPerSec) / this->config->maxVelocity_radPerSec, 1.0f);
                    const float32_t V = SIGN(this->velocitySetpointCurrent_radPerSec) * velocityPercent * this->config->maxVoltage_V;

                    lib_transforms_inversePark(
                        0.0f,
                        V,
                        rotorSin,
                        rotorCos,
                        &this->vAlpha_v,
                        &this->vBeta_v
                    );

                    SET_ENABLE(true, true, true);
                    break;
                }
            }
        }

        // [impl->fw~mc_016~1]
        lib_transforms_clarke(
            this->phaseCurrent_a[IO_BRIDGE_PHASE_U],
            this->phaseCurrent_a[IO_BRIDGE_PHASE_V],
            this->phaseCurrent_a[IO_BRIDGE_PHASE_W],
            &this->iAlpha_a,
            &this->iBeta_a
        );

        lib_transforms_park(
            this->iAlpha_a,
            this->iBeta_a,
            frameSin,
            frameCos,
            &this->iD_a,
            &this->iQ_a
        );

        // update outputs
        if (isVectorControl)
        {
            IO_bridge_setVoltageVector(this->config->bridge, this->vAlpha_v, this->vBeta_v);
        }
        else
        {
            IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_U, this->duty[IO_BRIDGE_PHASE_U]);
            IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_V, this->duty[IO_BRIDGE_PHASE_V]);
            IO_bridge_setPhaseDuty(this->config->bridge, IO_BRIDGE_PHASE_W, this->duty[IO_BRIDGE_PHASE_W]);
        }


        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_U, this->enable[IO_BRIDGE_PHASE_U]);
        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_V, this->enable[IO_BRIDGE_PHASE_V]);
        IO_bridge_setPhaseOutputEnabled(this->config->bridge, IO_BRIDGE_PHASE_W, this->enable[IO_BRIDGE_PHASE_W]);

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
        snapshot->selectedMode      = this->modeSelected;
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
        if (mode != APP_MOTORCONTROL_MODE_OFF)
        {
            this->modeSelected = mode;   // a method request selects it; OFF only disables
        }
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

// [impl->fw~mc_015~1]
void app_motorControl_run50us(void)
{
    for (size_t channel = 0U; channel < APP_MOTORCONTROL_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run50us();
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
