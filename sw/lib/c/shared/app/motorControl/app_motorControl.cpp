/* Includes */
#include <math.h>

#include "lib_utils.h"
#include "lib_timer.h"
#include "lib_pi.hpp"
#include "lib_transforms.h"
#include "HW_CORDIC.h"

#include "app_rotorEstimation.h"
#include "app_motorControl.h"

#include <array>
#include <atomic>


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
#define ALIGNMENT_DUTY_CYCLE (0.3f)

#define APP_MOTORCONTROL_MAX_DUTY_01 (0.9f)

// The 1 ms pass: the rate limits' time base (fw~mc_027).
#define RUN1MS_PERIOD_S (1.0e-3f)

// Field-oriented current loop (fw~mc_023): the discrete integral holds up to this w_bw T.
#define FOC_MAX_BANDWIDTH_PERIOD_PRODUCT (0.2f)

// In-module overcurrent trip (fw~safety_001). Each control cycle any phase
// current magnitude above OVERCURRENT_PHASE_TRIP_A, or a DC-bus current above
// OVERCURRENT_BUS_TRIP_A, latches the fault; the latch holds the bridge
// disabled until the fault-clear action (fw~mc_007).
#define OVERCURRENT_PHASE_TRIP_A (1.2f)
#define OVERCURRENT_BUS_TRIP_A   (1.5f)

// Encoder-fault trip (fw~safety_002). Commutation depends on rotor position, so
// more than this many CONSECUTIVE invalid encoder reads latches the fault; a
// valid read resets the count.
#define ENCODER_FAULT_LIMIT (5U)

/* Typedefs */

class Motor
{
    public:
        bool init(const app_motorControl_channelConfig_S * config);
        void run1ms();
        void run50us();

        bool getSnapshot(app_motorControl_snapshot_S * const snapshot);
        bool getRatings(app_motorControl_ratings_S * const ratings) const;
        void setVelocity(float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2);
        void setPositionSetpoint(float32_t position_rad, float32_t velocityLimit_radPerSec, float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec);
        void setTorque(float32_t torque_Nm, float32_t rateLimit_NmPerSec);
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
        void updateRotorPosition();   // the rotor estimate into the electrical angle
        void updateCoggingTorque();   // the cogging model at that angle

        void focTorqueLoop(const float32_t targetTorque_Nm, const theta_S * const rotorAngle);   // current regulator, voltage limit, inverse Park
        void focVelocityLoop(const float32_t targetVelocity_radPerSec, const theta_S * const rotorAngle);   // velocity PI to a torque demand, then the torque loop
        void focPositionLoop(const float32_t positionTarget_rad, const float32_t profileVelocity_radPerSec, const theta_S * const rotorAngle);   // position PI plus the profile velocity to a velocity command, then the velocity loop

        // called in run1ms()
        void updateOvercurrentLatch();
        void runMoveProfile();   // the position target and profile velocity (fw~mc_031)
        void latchFault();

        app_motorControl_channelConfig_S const * config;

        app_motorControl_mode_E modeCurrent;
        app_motorControl_mode_E modeRequested;
        app_motorControl_mode_E modeSelected;   // the method an enable runs; OFF keeps it
        app_motorControl_mode_E modeLastStep;   // ISR-owned: the method the last step ran
        DriveState driveState;

        // Each target: the value written with its rate limit, and the value in
        // effect that run1ms moves toward it (fw~mc_027).
        float32_t velocitySetpoint_radPerSec;
        float32_t velocitySetpointRequested_radPerSec;
        float32_t velocityRateLimit_radPerSec2;
        float32_t torqueSetpoint_Nm;
        float32_t torqueSetpointRequested_Nm;
        float32_t torqueRateLimit_NmPerSec;

        // the position setpoint (fw~mc_030): written by the API, planned from by run1ms
        enum class LastWrite : uint8_t { None, Position, Speed };
        struct PositionSetpoint
        {
            float32_t position_rad;               // theta_s
            float32_t velocityLimit_radPerSec;    // v, at most w_max (a zero write stands for w_max)
            float32_t accelLimit_radPerSec2;      // a; zero steps
            float32_t velocityAtPoint_radPerSec;  // |w_s|, at most v
        };
        PositionSetpoint positionSetpoint;
        volatile bool positionSetpointPending;   // a write run1ms has not yet planned from
        volatile LastWrite lastWrite;            // which of the setpoint and the speed target stands (fw~mc_031)

        // the move profile (fw~mc_031), run1ms-owned
        float32_t positionTarget_rad;             // theta*
        float32_t profileVelocity_radPerSec;      // w_f
        bool      profileArrived;                 // theta* reached theta_s: w_f held at w_s in the approach direction
        float32_t profileHeldVelocity_radPerSec;

        float32_t rotorAngle_rad;        // the estimator's mechanical angle, before the alignment offset
        float32_t mechanicalAngle_rad;   // rotorAngle_rad less the alignment offset
        float32_t magneticAngle_rad;
        float32_t magneticAngleTarget_rad;
        float32_t rotorPosition_rad;      // the multi-turn rotor position, zero at the enable (fw~mc_032)
        float32_t rotorPositionZero_rad;  // rotorAngle_rad at that zero
        float32_t rotorAnglePrev_rad;     // last cycle's rotorAngle_rad, for the seam crossings
        int32_t   rotorTurns;             // seam crossings of rotorAngle_rad since the zero

        float32_t rotorTorque_Nm;
        float32_t coggingTorque_Nm;      // T_cog at this cycle's rotor angle (fw~mc_028)

        float32_t velocityMeasured_radPerSec;   // the estimator's velocity
        float32_t unmodeledTorque_Nm;   // the estimator's Td

        bool isAligned;
        float32_t alignmentOffset_rad;
        lib_timer_channel_S alignmentDwell;
        lib_timer_channel_S prechargeDwell;

        bool isDriveAllowed;
        bool faultLatched;
        bool bridgeEnabled;   // last cycle's bridge output-enable, for the state view
        uint16_t encoderFaultCount;   // consecutive invalid encoder reads
        uint16_t faultLatches;        // latches since init, counted once each

        PiRegulator currentPi[APP_MOTORCONTROL_AXIS_COUNT];
        PiRegulator velocityPi;
        PiRegulator positionPi;

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
        bool enableApplied[IO_BRIDGE_PHASE_COUNT];   // what the bridge was last told; unchanged enables are not re-sent
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

// [impl->fw~mc_027~1] current toward target by at most rateLimit * dt; a zero
// limit steps.
static float32_t app_motorControl_private_slewToward(const float32_t current, const float32_t target,
                                                     const float32_t rateLimit, const float32_t dt_s)
{
    float32_t next = target;
    if (rateLimit > ZERO)
    {
        const float32_t maxStep = rateLimit * dt_s;
        const float32_t delta = target - current;
        if (fabsf(delta) > maxStep)
        {
            next = current + (SIGN(delta) * maxStep);
        }
    }
    return next;
}

// value saturated to +-magnitude; a non-finite value saturates to zero.
static float32_t app_motorControl_private_saturate(const float32_t value, const float32_t magnitude)
{
    float32_t out = ZERO;
    if (isfinite(value))
    {
        out = MIN_OF(fabsf(value), magnitude);
        out = (value < ZERO) ? -out : out;
    }
    return out;
}

// A negative or non-finite rate limit steps.
static float32_t app_motorControl_private_clampRateLimit(const float32_t rateLimit)
{
    return (rateLimit >= ZERO) ? rateLimit : ZERO;
}


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

// [impl->fw~mc_019~1] the estimator's per-cycle mechanical angle, less the
// alignment offset, times the pole pairs; its velocity is the drive's.
void Motor::updateRotorPosition()
{
    app_rotorEstimation_getState(this->config->rotorEstimator, &this->rotorAngle_rad, &this->velocityMeasured_radPerSec, &this->unmodeledTorque_Nm);
    this->mechanicalAngle_rad = this->rotorAngle_rad - this->alignmentOffset_rad;
    this->updateMagneticAngle();

    // [impl->fw~mc_032~1] turns counted at the seam crossings of the raw angle and
    // the position rebuilt from them each cycle, so no accumulation error at any travel
    const float32_t delta_rad = this->rotorAngle_rad - this->rotorAnglePrev_rad;
    if (delta_rad < -PI)
    {
        this->rotorTurns++;
    }
    else if (delta_rad > PI)
    {
        this->rotorTurns--;
    }
    this->rotorAnglePrev_rad = this->rotorAngle_rad;
    this->rotorPosition_rad = ((float32_t)this->rotorTurns * TWO_PI) + (this->rotorAngle_rad - this->rotorPositionZero_rad);
}

// [impl->fw~mc_028~1] the cogging model at the cycle's raw mechanical angle
void Motor::updateCoggingTorque()
{
    float32_t torque_Nm = ZERO;
    for (size_t i = 0U; i < APP_MOTORCONTROL_COG_HARMONICS; i++)
    {
        const app_motorControl_cogHarmonic_S * const harmonic = &this->config->foc.cogging[i];
        if (harmonic->amplitude_Nm > ZERO)
        {
            theta_S arg = { .sin_rad = ZERO, .cos_rad = 1.0f };
            const float32_t angle_rad = lib_transforms_wrapAngle(((float32_t)harmonic->order_perRev * this->rotorAngle_rad) + harmonic->phase_rad);
            (void) HW_CORDIC_sinCos(angle_rad, &arg);
            torque_Nm += harmonic->amplitude_Nm * arg.sin_rad;
        }
    }
    this->coggingTorque_Nm = torque_Nm;
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
            this->latchFault();
        }
    }

    if (IO_bridge_getBusCurrent(this->config->bridge, &amps))
    {
        this->busCurrent = amps;
        if (fabsf(amps) > OVERCURRENT_BUS_TRIP_A)
        {
            this->latchFault();
        }
    }
}

// [impl->fw~ops_002~1] a latch counts once, so a reader that misses the
// latched state between its passes still sees the count move.
void Motor::latchFault()
{
    if (!this->faultLatched)
    {
        this->faultLatched = true;
        this->faultLatches++;
    }
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


void Motor::focTorqueLoop(const float32_t targetTorque_Nm, const theta_S * const rotorAngle)
{
    const float32_t targetDCurrent_a = 0.0f;
    // [impl->fw~mc_028~1] the q-axis torque command: the method's demand less
    // the compensated cogging, clamped to the current limit
    const float32_t torqueLimit_Nm = this->config->foc.torqueConstant_NmPerA * this->config->foc.maxCurrent_A;
    const float32_t qTorque_Nm = app_motorControl_private_saturate(
        targetTorque_Nm - (this->config->foc.coggingKff * this->coggingTorque_Nm), torqueLimit_Nm);
    const float32_t targetQCurrent_a = qTorque_Nm / this->config->foc.torqueConstant_NmPerA;

    // [impl->fw~mc_023~1]
    const float32_t errorDCurrent_a = targetDCurrent_a - this->iD_a;
    float32_t vD = this->currentPi[APP_MOTORCONTROL_AXIS_D].step(errorDCurrent_a);

    const float32_t errorQCurrent_a = targetQCurrent_a - this->iQ_a;
    float32_t vQ = this->currentPi[APP_MOTORCONTROL_AXIS_Q].step(errorQCurrent_a);

    // [impl->fw~mc_024~1]
    const float32_t vLim = MIN_OF(this->config->maxVoltage_V, (this->busVoltage * LIB_TRANSFORMS_1_DIV_SQRT3));
    const float32_t vVectorMag = sqrtf((vD*vD) + (vQ*vQ));
    if (vVectorMag > vLim)
    {
        const float32_t k = vLim / vVectorMag; // limit / mag
        vD *= k;
        vQ *= k;

        this->currentPi[APP_MOTORCONTROL_AXIS_D].backCalculate(errorDCurrent_a, vD);
        this->currentPi[APP_MOTORCONTROL_AXIS_Q].backCalculate(errorQCurrent_a, vQ);
    }

    lib_transforms_inversePark(
        vD,
        vQ,
        *rotorAngle,
        &this->vAlpha_v,
        &this->vBeta_v
    );

    SET_ENABLE(true, true, true);
}

// [impl->fw~mc_025~1]
void Motor::focVelocityLoop(const float32_t targetVelocity_radPerSec, const theta_S * const rotorAngle)
{
    const float32_t velocityError_radPerSec = targetVelocity_radPerSec - this->velocityMeasured_radPerSec;
    const float32_t disturbanceFeedForward_Nm = this->unmodeledTorque_Nm * this->config->foc.velocityKff;
    float32_t targetTorque_Nm = this->velocityPi.step(velocityError_radPerSec) - disturbanceFeedForward_Nm;

    const float32_t torqueLimit_Nm = this->config->foc.torqueConstant_NmPerA * this->config->foc.maxCurrent_A;
    if (fabsf(targetTorque_Nm) > torqueLimit_Nm)
    {
        targetTorque_Nm = SIGN(targetTorque_Nm) * torqueLimit_Nm;

        // the integral is solved with the feed-forward still in the demand
        this->velocityPi.backCalculate(velocityError_radPerSec, (targetTorque_Nm + disturbanceFeedForward_Nm));
    }

    this->focTorqueLoop(targetTorque_Nm, rotorAngle);
}

// [impl->fw~mc_029~1] w_c = K_pp e + K_pi S_p + w_f, clamped to +/-w_max with the
// integral held through the clamp, onto the velocity method's step. Not
// back-calculated: with w_f itself at w_max that would wind S_p against the
// proportional term and leave the loop limp when the profile stops.
void Motor::focPositionLoop(const float32_t positionTarget_rad, const float32_t profileVelocity_radPerSec, const theta_S * const rotorAngle)
{
    const float32_t positionError_rad = positionTarget_rad - this->rotorPosition_rad;
    float32_t velocityCommand_radPerSec = this->positionPi.step(positionError_rad) + profileVelocity_radPerSec;

    if (fabsf(velocityCommand_radPerSec) > this->config->maxVelocity_radPerSec)
    {
        velocityCommand_radPerSec = SIGN(velocityCommand_radPerSec) * this->config->maxVelocity_radPerSec;
        this->positionPi.hold(positionError_rad);
    }
    this->focVelocityLoop(velocityCommand_radPerSec, rotorAngle);
}

// [impl->fw~mc_031~1] theta* advances by w_f each pass, saturated to +/-theta_max; w_f
// from whichever of the position setpoint and the speed target was written last:
// the minimum-time profile to theta_s arriving at w_s, or the jog.
void Motor::runMoveProfile()
{
    const float32_t T = RUN1MS_PERIOD_S;
    float32_t omegaF = ZERO;

    if (this->positionSetpointPending)
    {
        this->positionSetpointPending = false;
        this->profileArrived = false;
    }

    switch (this->lastWrite)
    {
        case LastWrite::Speed:
            omegaF = this->velocitySetpoint_radPerSec;
            break;

        case LastWrite::Position:
        {
            if (this->profileArrived)
            {
                omegaF = this->profileHeldVelocity_radPerSec;
            }
            else
            {
                const PositionSetpoint sp = this->positionSetpoint;
                const float32_t distance_rad = sp.position_rad - this->positionTarget_rad;
                const float32_t direction = (distance_rad < ZERO) ? -1.0f : 1.0f;
                const float32_t remaining_rad = fabsf(distance_rad);
                if (sp.accelLimit_radPerSec2 > ZERO)
                {
                    // The speed from which braking at a, one a T step per pass, reaches w_s
                    // exactly at theta_s; w_f moves toward it, or toward v, at a per second.
                    const float32_t aT = sp.accelLimit_radPerSec2 * T;
                    const float32_t halfAT = 0.5f * aT;
                    const float32_t ws = sp.velocityAtPoint_radPerSec;
                    const float32_t radicand = MAX_OF(ZERO, (halfAT * halfAT) + (ws * ws) - (aT * ws) + (2.0f * sp.accelLimit_radPerSec2 * remaining_rad));
                    const float32_t speed = MIN_OF(sp.velocityLimit_radPerSec, sqrtf(radicand) - halfAT);
                    omegaF = app_motorControl_private_slewToward(this->profileVelocity_radPerSec, direction * speed, sp.accelLimit_radPerSec2, T);
                }
                else
                {
                    omegaF = direction * sp.velocityLimit_radPerSec;
                }

                // this pass reaches theta_s while closing on it: on it at rest, or past it at w_s
                if (((omegaF * direction) >= ZERO) && (remaining_rad <= (fabsf(omegaF) * T)))
                {
                    this->profileArrived = true;
                    this->profileHeldVelocity_radPerSec = direction * sp.velocityAtPoint_radPerSec;
                    omegaF = this->profileHeldVelocity_radPerSec;
                    if (sp.velocityAtPoint_radPerSec == ZERO)
                    {
                        this->positionTarget_rad = sp.position_rad;
                    }
                }
            }
            break;
        }

        default:
        case LastWrite::None:
            break;
    }

    // w_f zero while it would push theta* past a bound
    float32_t next_rad = this->positionTarget_rad + (omegaF * T);
    const float32_t bound_rad = this->config->foc.maxPosition_rad;
    if (next_rad > bound_rad)
    {
        next_rad = bound_rad;
        omegaF = ZERO;
    }
    else if (next_rad < -bound_rad)
    {
        next_rad = -bound_rad;
        omegaF = ZERO;
    }
    this->positionTarget_rad = next_rad;
    this->profileVelocity_radPerSec = omegaF;
}

/* Public Class Function Definitions */
bool Motor::init(const app_motorControl_channelConfig_S * channelConfig)
{
    bool isValid = true;

    isValid &= (channelConfig->gateDriver < DEV_GATEDRIVER_CHANNEL_COUNT);
    isValid &= (channelConfig->bridge < IO_BRIDGE_CHANNEL_COUNT);
    isValid &= (channelConfig->rotorEstimator < APP_ROTORESTIMATION_CHANNEL_COUNT);
    isValid &= (channelConfig->encoder < IO_AS5048_CHANNEL_COUNT);
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
    // [impl->fw~mc_022~1] I_max under the phase trip
    isValid &= (channelConfig->foc.maxCurrent_A > ZERO);
    isValid &= (channelConfig->foc.maxCurrent_A < OVERCURRENT_PHASE_TRIP_A);
    // [impl->fw~mc_023~1] R, L, w_bw nonzero, w_bw T bounded: K_p, K_i follow
    isValid &= (channelConfig->foc.motorR_ohm > ZERO);
    isValid &= (channelConfig->foc.motorL_H > ZERO);
    isValid &= (channelConfig->foc.currentLoopBandwidth_radPerSec > ZERO);
    isValid &= ((channelConfig->foc.currentLoopBandwidth_radPerSec * channelConfig->pwmPeriod_s) <= FOC_MAX_BANDWIDTH_PERIOD_PRODUCT);
    // [impl->fw~mc_025~1] K_t is a divisor; the velocity gains positive, k_ff in [0, 1]
    isValid &= (channelConfig->foc.torqueConstant_NmPerA > ZERO);
    isValid &= (channelConfig->foc.velocityKp_NmPerRadPerSec > ZERO);
    isValid &= (channelConfig->foc.velocityKi_NmPerRad > ZERO);
    isValid &= ((channelConfig->foc.velocityKff >= ZERO) && (channelConfig->foc.velocityKff <= 1.0f));
    // [impl->fw~mc_028~1] k_cog in [0, 1]; the cogging amplitudes at or above zero
    isValid &= ((channelConfig->foc.coggingKff >= ZERO) && (channelConfig->foc.coggingKff <= 1.0f));
    for (size_t i = 0U; i < APP_MOTORCONTROL_COG_HARMONICS; i++)
    {
        isValid &= (channelConfig->foc.cogging[i].amplitude_Nm >= ZERO);
    }
    // [impl->fw~mc_029~1] the position gains positive; [impl->fw~mc_031~1] the travel bound positive
    isValid &= (channelConfig->foc.positionKp_perSec > ZERO);
    isValid &= (channelConfig->foc.positionKi_perSec2 > ZERO);
    isValid &= (channelConfig->foc.maxPosition_rad > ZERO);

    if (isValid)
    {
        this->config = channelConfig;

        this->modeCurrent = APP_MOTORCONTROL_MODE_OFF;
        this->modeRequested = APP_MOTORCONTROL_MODE_OFF;
        this->modeSelected = channelConfig->defaultMode;
        this->modeLastStep = APP_MOTORCONTROL_MODE_OFF;
        this->driveState = DriveState::Off;

        this->isAligned = false;
        this->alignmentOffset_rad = ZERO;
        lib_timer_init(&this->alignmentDwell, LIB_TIMER_PRECISION_MS, ALIGNMENT_DWELL_TIMER_MS);
        lib_timer_init(&this->prechargeDwell, LIB_TIMER_PRECISION_MS, channelConfig->prechargeTime_ms);

        this->faultLatched = false;
        this->faultLatches = 0U;
        this->bridgeEnabled = false;
        this->encoderFaultCount = 0U;
        for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
        {
            this->enableApplied[phase] = true;   // the bridge's init leaves every phase enabled; the first cycle disables them
        }

        this->rotorAngle_rad = ZERO;
        this->mechanicalAngle_rad = ZERO;
        this->magneticAngle_rad = ZERO;
        this->rotorPosition_rad = ZERO;
        this->rotorPositionZero_rad = ZERO;
        this->rotorAnglePrev_rad = ZERO;
        this->rotorTurns = 0;
        this->velocityMeasured_radPerSec = ZERO;
        this->coggingTorque_Nm = ZERO;

        this->velocitySetpoint_radPerSec = ZERO;
        this->velocitySetpointRequested_radPerSec = ZERO;
        this->velocityRateLimit_radPerSec2 = ZERO;
        this->torqueSetpoint_Nm = ZERO;
        this->torqueSetpointRequested_Nm = ZERO;
        this->torqueRateLimit_NmPerSec = ZERO;

        this->positionSetpoint = PositionSetpoint{};
        this->positionSetpointPending = false;
        this->lastWrite = LastWrite::None;
        this->positionTarget_rad = ZERO;
        this->profileVelocity_radPerSec = ZERO;
        this->profileArrived = false;
        this->profileHeldVelocity_radPerSec = ZERO;

        PiConfig piConfig =
        {
            .kp = channelConfig->foc.motorL_H * channelConfig->foc.currentLoopBandwidth_radPerSec,
            .ki = channelConfig->foc.motorR_ohm * channelConfig->foc.currentLoopBandwidth_radPerSec,
            .period_s = channelConfig->pwmPeriod_s,
        };
        for (size_t axis = 0U; axis < APP_MOTORCONTROL_AXIS_COUNT; axis++)
        {
            this->currentPi[axis].init(piConfig);
        }

        piConfig.kp = channelConfig->foc.velocityKp_NmPerRadPerSec;
        piConfig.ki = channelConfig->foc.velocityKi_NmPerRad;
        this->velocityPi.init(piConfig);


        piConfig.kp = channelConfig->foc.positionKp_perSec;
        piConfig.ki = channelConfig->foc.positionKi_perSec2;
        this->positionPi.init(piConfig);
    }
    return isValid;
}

void Motor::run1ms()
{
    if (this->config != NULL)
    {
        // The rotor estimate as it stands this pass, for the alignment capture
        // below; the ISR owns the estimate members.
        float32_t rotorAngle_rad = ZERO;
        float32_t velocity_radPerSec = ZERO;
        float32_t unmodeledTorque_Nm = ZERO;
        app_rotorEstimation_getState(this->config->rotorEstimator, &rotorAngle_rad, &velocity_radPerSec, &unmodeledTorque_Nm);

        // [impl->fw~safety_002~1] Latch a fault on a persistently invalid
        // encoder — a stale position would commutate the live bridge wrong.
        IO_AS5048_status_E encoderStatus = IO_AS5048_STATUS_IDLE;
        (void)IO_AS5048_getStatus(this->config->encoder, &encoderStatus);
        if (encoderStatus == IO_AS5048_STATUS_FAULT)
        {
            this->encoderFaultCount++;
            if (this->encoderFaultCount > ENCODER_FAULT_LIMIT)
            {
                this->latchFault();
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

        const DriveState lastState = this->driveState;
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

        // [impl->fw~mc_027~1] the targets in effect: zero while dark, seeded at
        // the enable (speed from the rotor estimate, torque from zero), then
        // moved toward the written values at their rate limits every pass.
        if (this->driveState == DriveState::Off)
        {
            this->velocitySetpoint_radPerSec = ZERO;
            this->torqueSetpoint_Nm = ZERO;
            // [impl->fw~mc_026~1] dark drops the torque request: the next enable
            // starts from a fresh write, never a stale torque
            this->torqueSetpointRequested_Nm = ZERO;
            this->torqueRateLimit_NmPerSec = ZERO;
            if (lastState != DriveState::Off)
            {
                // [impl->fw~mc_030~1] a disable or fault drops the position setpoint
                // once; a write while dark is held to the enable
                this->positionSetpoint = PositionSetpoint{};
                this->positionSetpointPending = false;
                this->lastWrite = LastWrite::None;
            }
        }
        else if (lastState == DriveState::Off)
        {
            this->velocitySetpoint_radPerSec = velocity_radPerSec;
            this->torqueSetpoint_Nm = ZERO;
        }
        else
        {
            this->velocitySetpoint_radPerSec = app_motorControl_private_slewToward(
                this->velocitySetpoint_radPerSec, this->velocitySetpointRequested_radPerSec,
                this->velocityRateLimit_radPerSec2, RUN1MS_PERIOD_S);
            this->torqueSetpoint_Nm = app_motorControl_private_slewToward(
                this->torqueSetpoint_Nm, this->torqueSetpointRequested_Nm,
                this->torqueRateLimit_NmPerSec, RUN1MS_PERIOD_S);
        }

        // [impl->fw~mc_031~1] theta* and w_f zero until the method's first step after the enable
        if (this->driveState == DriveState::Driving)
        {
            this->runMoveProfile();
        }
        else
        {
            this->positionTarget_rad = ZERO;
            this->profileVelocity_radPerSec = ZERO;
            this->profileArrived = false;
        }

        switch (this->driveState)
        {
            case DriveState::Off:
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
                // the dwell; the estimator's angle there is electrical zero.
                SET_DUTY_AND_ENABLE(ALIGNMENT_DUTY_CYCLE, ZERO, ZERO, true, true, true);
                if (lib_timer_runTimerWithEnable(&this->alignmentDwell, true) == LIB_TIMER_STATE_EXPIRED)
                {
                    this->alignmentOffset_rad = rotorAngle_rad;
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
        this->updateCoggingTorque();

        // One sine/cosine pair of the rotor angle serves every transform this cycle.
        theta_S rotorAngle = { .sin_rad = ZERO, .cos_rad = 1.0f };
        (void) HW_CORDIC_sinCos(this->magneticAngle_rad, &rotorAngle);

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
            rotorAngle,
            &this->iD_a,
            &this->iQ_a
        );

        const bool isFOC = ((this->modeCurrent == APP_MOTORCONTROL_MODE_FOC_TORQUE) ||
                            (this->modeCurrent == APP_MOTORCONTROL_MODE_FOC_VELOCITY) ||
                            (this->modeCurrent == APP_MOTORCONTROL_MODE_FOC_POSITION));

        // [impl->fw~mc_016~1] the estimator's known torque: K_t I_q plus, while a
        // field-oriented method drives, the compensated share of the cogging model
        this->rotorTorque_Nm = this->iQ_a * this->config->foc.torqueConstant_NmPerA;
        const bool isCompensating = (isFOC && (this->driveState == DriveState::Driving));
        const float32_t knownCogging_Nm = isCompensating ? (this->config->foc.coggingKff * this->coggingTorque_Nm) : ZERO;
        app_rotorEstimation_setTorqueInput(this->config->rotorEstimator, (this->rotorTorque_Nm + knownCogging_Nm));

        bool isAnyPhaseEnabled = (this->driveState != DriveState::Off);
        bool isVectorControl = false;

        // Integrals are zero at a method's first step: a method change while
        // driving (torque <-> velocity) does not pass through Off.
        const bool isMethodChange = (this->modeCurrent != this->modeLastStep);
        this->modeLastStep = this->modeCurrent;

        if ((this->driveState != DriveState::Driving) || (!isFOC) || isMethodChange)
        {
            this->currentPi[APP_MOTORCONTROL_AXIS_D].reset();
            this->currentPi[APP_MOTORCONTROL_AXIS_Q].reset();
        }
        if ((this->driveState != DriveState::Driving) ||
            (!isFOC) || isMethodChange)
        {
            this->velocityPi.reset();
            this->positionPi.reset();
        }
        // [impl->fw~mc_032~1] the position is zero until the first commutation step after the enable
        if (this->driveState != DriveState::Driving)
        {
            this->rotorTurns = 0;
            this->rotorPositionZero_rad = this->rotorAngle_rad;
            this->rotorPosition_rad = ZERO;
        }

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
                    // [impl->fw~mc_011~1]
                    // Commutate: apply the sector pattern for the rotor
                    // electrical angle advanced by the lead, at a duty
                    // proportional to the target magnitude, clamped to max.

                    const float32_t lead_rad = (this->velocitySetpoint_radPerSec >= ZERO) ? RAD_90DEG : -RAD_90DEG;
                    const float32_t target = lib_transforms_wrapAngle(this->magneticAngle_rad + lead_rad);
                    this->magneticAngleTarget_rad = target;

                    // TODO - compute duty cycle from velocity error
                    const float32_t duty01 = MIN_OF(fabsf(this->velocitySetpoint_radPerSec) / this->config->maxVelocity_radPerSec, APP_MOTORCONTROL_MAX_DUTY_01);

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
                    // compute rotor frame voltage
                    const float32_t velocityPercent = MIN_OF(fabsf(this->velocitySetpoint_radPerSec) / this->config->maxVelocity_radPerSec, 1.0f);
                    const float32_t V = SIGN(this->velocitySetpoint_radPerSec) * velocityPercent * this->config->maxVoltage_V;

                    lib_transforms_inversePark(
                        0.0f,
                        V,
                        rotorAngle,
                        &this->vAlpha_v,
                        &this->vBeta_v
                    );

                    SET_ENABLE(true, true, true);
                    break;
                }

                case APP_MOTORCONTROL_MODE_FOC_TORQUE:
                {
                    isVectorControl = true;
                    // [impl->fw~mc_022~1] i_q* = T_t / K_t, in the torque loop
                    this->focTorqueLoop(this->torqueSetpoint_Nm, &rotorAngle);
                    break;
                }

                case APP_MOTORCONTROL_MODE_FOC_VELOCITY:
                {
                    isVectorControl = true;
                    this->focVelocityLoop(this->velocitySetpoint_radPerSec, &rotorAngle);
                    break;
                }

                case APP_MOTORCONTROL_MODE_FOC_POSITION:
                {
                    isVectorControl = true;
                    this->focPositionLoop(this->positionTarget_rad, this->profileVelocity_radPerSec, &rotorAngle);
                    break;
                }
            }
        }

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

        for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
        {
            if (this->enable[phase] != this->enableApplied[phase])
            {
                IO_bridge_setPhaseOutputEnabled(this->config->bridge, (IO_bridge_phase_E) phase, this->enable[phase]);
                this->enableApplied[phase] = this->enable[phase];
            }
        }

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
        snapshot->velocitySetpoint_radPerSec = this->velocitySetpoint_radPerSec;
        snapshot->torqueTarget_Nm   = this->torqueSetpoint_Nm;
        snapshot->velocityMeasured_radPerSec = this->velocityMeasured_radPerSec;
        snapshot->positionTarget_rad = this->positionTarget_rad;
        snapshot->profileVelocity_radPerSec = this->profileVelocity_radPerSec;
        snapshot->rotorPosition_rad = this->rotorPosition_rad;
        snapshot->faultLatches      = this->faultLatches;

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

// [impl->fw~conn_server_008~1]
bool Motor::getRatings(app_motorControl_ratings_S * const ratings) const
{
    bool ret = false;
    if ((this->config != NULL) && (ratings != NULL))
    {
        ratings->torqueConstant_NmPerA  = this->config->foc.torqueConstant_NmPerA;
        ratings->maxCurrent_A           = this->config->foc.maxCurrent_A;
        ratings->maxTorque_Nm           = this->config->foc.torqueConstant_NmPerA * this->config->foc.maxCurrent_A;
        ratings->maxVelocity_radPerSec  = this->config->maxVelocity_radPerSec;
        ratings->maxPosition_rad        = this->config->foc.maxPosition_rad;
        ret = true;
    }
    return ret;
}

void Motor::setVelocity(float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2)
{
    if (this->config != NULL)
    {
        // Limit before value: the 1 ms task can run between the two stores, and
        // the old value under the new limit is harmless where the reverse skips a ramp.
        this->velocityRateLimit_radPerSec2 = app_motorControl_private_clampRateLimit(rateLimit_radPerSec2);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        this->velocitySetpointRequested_radPerSec = app_motorControl_private_saturate(velocity_radPerSec, this->config->maxVelocity_radPerSec);

        if (this->modeSelected == APP_MOTORCONTROL_MODE_FOC_POSITION)
        {
            this->lastWrite = LastWrite::Speed;   // [impl->fw~mc_031~1] a speed target under the position method jogs
        }
        if (this->modeSelected == APP_MOTORCONTROL_MODE_FOC_TORQUE)
        {
            // [impl->fw~mc_026~1] under the torque method a speed target is its
            // fraction of full torque, its rate limit scaled alike
            const float32_t torquePerSpeed_NmPerRadPerSec = (this->config->foc.torqueConstant_NmPerA * this->config->foc.maxCurrent_A) / this->config->maxVelocity_radPerSec;
            this->setTorque(this->velocitySetpointRequested_radPerSec * torquePerSpeed_NmPerRadPerSec,
                            this->velocityRateLimit_radPerSec2 * torquePerSpeed_NmPerRadPerSec);
        }
    }
}

// [impl->fw~mc_030~1] the most recent write stands: theta_s saturated to +/-theta_max,
// v to w_max (zero, negative or non-finite standing for w_max), a non-finite or
// negative stepping, |w_s| at most v
void Motor::setPositionSetpoint(float32_t position_rad, float32_t velocityLimit_radPerSec, float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec)
{
    if (this->config != NULL)
    {
        PositionSetpoint sp = {};
        sp.position_rad = app_motorControl_private_saturate(position_rad, this->config->foc.maxPosition_rad);
        sp.velocityLimit_radPerSec = ((isfinite(velocityLimit_radPerSec)) && (velocityLimit_radPerSec > ZERO))
                                         ? MIN_OF(velocityLimit_radPerSec, this->config->maxVelocity_radPerSec)
                                         : this->config->maxVelocity_radPerSec;
        sp.accelLimit_radPerSec2 = app_motorControl_private_clampRateLimit(accelLimit_radPerSec2);
        sp.velocityAtPoint_radPerSec = fabsf(app_motorControl_private_saturate(velocityAtPoint_radPerSec, sp.velocityLimit_radPerSec));
        this->positionSetpoint = sp;
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the fields before the flags, as setVelocity
        this->positionSetpointPending = true;
        this->lastWrite = LastWrite::Position;
    }
}

// [impl->fw~mc_026~1]
void Motor::setTorque(float32_t torque_Nm, float32_t rateLimit_NmPerSec)
{
    if (this->config != NULL)
    {
        const float32_t torqueLimit_Nm = this->config->foc.torqueConstant_NmPerA * this->config->foc.maxCurrent_A;
        this->torqueRateLimit_NmPerSec = app_motorControl_private_clampRateLimit(rateLimit_NmPerSec);
        std::atomic_signal_fence(std::memory_order_seq_cst);   // limit before value, as setVelocity
        this->torqueSetpointRequested_Nm = app_motorControl_private_saturate(torque_Nm, torqueLimit_Nm);
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

bool app_motorControl_getRatings(app_motorControl_channel_E channel, app_motorControl_ratings_S * const ratings)
{
    bool ret = false;
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        ret = data->channels[channel].getRatings(ratings);
    }
    return ret;
}

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].setVelocity(velocity_radPerSec, rateLimit_radPerSec2);
    }
}

void app_motorControl_setTorque(app_motorControl_channel_E channel, float32_t torque_Nm, float32_t rateLimit_NmPerSec)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].setTorque(torque_Nm, rateLimit_NmPerSec);
    }
}

void app_motorControl_setPositionSetpoint(app_motorControl_channel_E channel, float32_t position_rad, float32_t velocityLimit_radPerSec,
                                          float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec)
{
    if (channel < APP_MOTORCONTROL_CHANNEL_COUNT)
    {
        data->channels[channel].setPositionSetpoint(position_rad, velocityLimit_radPerSec, accelLimit_radPerSec2, velocityAtPoint_radPerSec);
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
