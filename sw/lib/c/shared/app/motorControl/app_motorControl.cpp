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

#define RAD_60DEG DEG_TO_RAD(60.0f)
#define RAD_90DEG DEG_TO_RAD(90.0f)
#define RAD_120DEG DEG_TO_RAD(120.0f)
#define RAD_180DEG DEG_TO_RAD(180.0f)
#define RAD_240DEG DEG_TO_RAD(240.0f)
#define RAD_300DEG DEG_TO_RAD(300.0f)

#define APP_MOTORCONTROL_MAX_DUTY_01 (0.9f)

// The 1 ms pass: the rate limits' time base (fw~mc_027).
#define APP_MOTORCONTROL_RUN1MS_PERIOD_S (1.0e-3f)

// Field-oriented current loop (fw~mc_023): the discrete integral holds up to this w_bw T.
#define APP_MOTORCONTROL_FOC_MAX_BANDWIDTH_PERIOD_PRODUCT (0.2f)

// A back-calculated command returns to V_lim only to float rounding; within this share it is at the limit.
#define APP_MOTORCONTROL_VOLTAGE_LIMIT_ROUNDING (1.0e-5f)

/* Typedefs */

class Motor
{
    public:
        bool init(const app_motorControl_channelConfig_S * config);
        void run1ms();
        void run50us(IO_bridge_channel_E bridge);

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

        enum Axis : uint8_t { AxisD, AxisQ, AxisCount };   // indexes the current regulators

        // One row per mode: a new method is a row and its step.
        struct Method
        {
            bool isFieldOriented;   // current regulators and cogging compensation (fw~mc_023, fw~mc_028)
            bool isVectorOutput;    // the step commands a voltage vector, else per-phase duties
            bool usesProfile;       // the move profile runs (fw~mc_031)
            void (Motor::*step)(const lib_transforms_sinCos_S * rotorAngle);
        };
        static const Method methods[];

        // per PWM cycle, in run50us's order
        void updateRotorPosition();   // the rotor estimate into the electrical angle
        void updateMagneticAngle();
        void updateCoggingTorque();   // the cogging model at that angle
        void updateCyclePositionTarget();   // theta* between the 1 ms passes (fw~mc_031)
        void measureCurrents(const lib_transforms_sinCos_S * rotorAngle);
        void resetOnEntry(bool isDriving, bool isFieldOriented);
        void stepSixStep(const lib_transforms_sinCos_S * rotorAngle);
        void stepSinusoidal(const lib_transforms_sinCos_S * rotorAngle);
        void stepFocTorque(const lib_transforms_sinCos_S * rotorAngle);
        void stepFocVelocity(const lib_transforms_sinCos_S * rotorAngle);
        void stepFocPosition(const lib_transforms_sinCos_S * rotorAngle);
        void applyOutputs(bool isVectorOutput, bool isBridgeOn);

        // Each returns true when the cycle's command is at the voltage limit (fw~mc_024).
        bool focTorqueLoop(float32_t targetTorque_Nm, const lib_transforms_sinCos_S * rotorAngle);   // current regulator, voltage limit, inverse Park
        bool focVelocityLoop(float32_t targetVelocity_radPerSec, const lib_transforms_sinCos_S * rotorAngle);   // velocity PI to a torque demand, then the torque loop
        bool focPositionLoop(float32_t positionTarget_rad, float32_t profileVelocity_radPerSec, const lib_transforms_sinCos_S * rotorAngle);   // position PI plus the profile velocity, then the velocity loop

        void setEnables(bool enableU, bool enableV, bool enableW);
        void setPattern(float32_t dutyU, float32_t dutyV, float32_t dutyW, bool enableU, bool enableV, bool enableW);

        // per 1 ms pass, in run1ms's order
        void updateFaultLatches();
        void updateOvercurrentLatch();
        void latchFault();
        app_motorControl_mode_E decideMode(DriveState lastState) const;
        DriveState decideDriveState(DriveState lastState, app_motorControl_mode_E mode);
        void slewTargets(DriveState lastState, DriveState state, float32_t estimatedVelocity_radPerSec);
        void updateMoveProfile(DriveState state, app_motorControl_mode_E mode);
        void runMoveProfile();   // the position target and profile velocity (fw~mc_031)
        float32_t planPositionMove();   // w_f toward the position setpoint
        void applyStateOutputs(DriveState state, float32_t estimatedAngle_rad);

        app_motorControl_channelConfig_S const * config;
        float32_t torqueLimit_Nm;                 // K_t I_max
        float32_t inverseTorqueConstant_APerNm;   // 1 / K_t

        app_motorControl_mode_E modeCurrent;
        app_motorControl_mode_E modeRequested;
        app_motorControl_mode_E modeSelected;   // the method an enable runs; OFF keeps it
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
        LastWrite lastWriteApplied;              // run1ms-owned: lastWrite as of the last pass

        // the move profile (fw~mc_031), run1ms-owned
        float32_t positionTarget_rad;             // theta*
        float32_t profileVelocity_radPerSec;      // w_f
        bool      profileArrived;                 // theta* reached theta_s: w_f held at w_s in the approach direction
        float32_t profileHeldVelocity_radPerSec;
        volatile uint32_t profilePasses;          // bumped after each pass publishes theta* and w_f

        // ISR-owned: theta* each cycle, from the last two passes' theta* (fw~mc_031)
        uint32_t  profilePassesSeen;
        uint32_t  cyclesSincePass;
        float32_t cycleTargetFrom_rad;
        float32_t cycleTargetTo_rad;
        float32_t cycleProfileVelocity_radPerSec;
        float32_t cyclePositionTarget_rad;
        float32_t pwmPeriodPerPass;               // T / 1 ms: one cycle's share of a pass

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
        volatile bool bridgeEnabled;   // the bridge output enable as last written, by the cycle
        uint16_t encoderFaultCount;   // consecutive invalid encoder reads
        uint16_t faultLatches;        // latches since init, counted once each

        PiRegulator currentPi[AxisCount];
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

/* Private Data Definitions */
constinit static app_motorControl_data_S app_motorControl_data = {};
constinit static app_motorControl_data_S * const data = &app_motorControl_data;

// In app_motorControl_mode_E order.
const Motor::Method Motor::methods[] =
{
    { false, false, false, nullptr },                      // OFF
    { false, false, false, &Motor::stepSixStep },          // SIX_STEP_TRAP
    { false, true,  false, &Motor::stepSinusoidal },       // SINUSOIDAL
    { true,  true,  false, &Motor::stepFocTorque },        // FOC_TORQUE
    { true,  true,  false, &Motor::stepFocVelocity },      // FOC_VELOCITY
    { true,  true,  true,  &Motor::stepFocPosition },      // FOC_POSITION
};

/* Private Free Function Definitions */

// [impl->fw~mc_027~1] current toward target by at most rateLimit * dt; a zero
// limit steps.
static float32_t app_motorControl_private_slewToward(float32_t current, float32_t target, float32_t rateLimit, float32_t dt_s)
{
    float32_t next = target;
    if (rateLimit > ZERO)
    {
        const float32_t maxStep = rateLimit * dt_s;
        const float32_t delta = target - current;
        if (fabsf(delta) > maxStep)
        {
            next = current + (SGN(delta) * maxStep);
        }
    }
    return next;
}

// value saturated to +-magnitude; a non-finite value saturates to zero.
static float32_t app_motorControl_private_saturate(float32_t value, float32_t magnitude)
{
    float32_t out = ZERO;
    if (isfinite(value))
    {
        out = SATURATE(value, -magnitude, magnitude);
    }
    return out;
}

// A negative or non-finite rate limit steps.
static float32_t app_motorControl_private_clampRateLimit(float32_t rateLimit)
{
    return ((isfinite(rateLimit)) && (rateLimit > ZERO)) ? rateLimit : ZERO;
}


/* Private Class Function Definitions: the PWM cycle */

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

void Motor::updateMagneticAngle()
{
    this->magneticAngle_rad = lib_transforms_wrapAngle(this->mechanicalAngle_rad * this->config->motorPolePairs);
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
            lib_transforms_sinCos_S arg = { .sin = ZERO, .cos = 1.0f };
            const float32_t angle_rad = lib_transforms_wrapAngle(((float32_t)harmonic->order_perRev * this->rotorAngle_rad) + harmonic->phase_rad);
            (void) HW_CORDIC_sinCos(angle_rad, &arg);
            torque_Nm += harmonic->amplitude_Nm * arg.sin;
        }
    }
    this->coggingTorque_Nm = torque_Nm;
}

// [impl->fw~mc_031~1] theta* each cycle on the line from the previous pass's theta* to
// the latest's, reaching it on the pass's last cycle; a pass's values are taken whole.
void Motor::updateCyclePositionTarget()
{
    const uint32_t passes = this->profilePasses;
    if (passes != this->profilePassesSeen)
    {
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the count before the values it publishes
        this->profilePassesSeen = passes;
        this->cyclesSincePass = 0U;
        this->cycleTargetFrom_rad = this->cycleTargetTo_rad;
        this->cycleTargetTo_rad = this->positionTarget_rad;
        this->cycleProfileVelocity_radPerSec = this->profileVelocity_radPerSec;
    }
    if (this->cyclesSincePass < UINT16_MAX)
    {
        this->cyclesSincePass++;
    }
    const float32_t share = MIN_OF(((float32_t)this->cyclesSincePass * this->pwmPeriodPerPass), 1.0f);
    this->cyclePositionTarget_rad = this->cycleTargetFrom_rad + ((this->cycleTargetTo_rad - this->cycleTargetFrom_rad) * share);
}

// [impl->fw~mc_016~1] the cycle's phase currents into the rotor frame, and the
// estimator's known torque: K_t I_q once aligned (the Park frame is arbitrary
// before) plus the compensated share of the cogging model
void Motor::measureCurrents(const lib_transforms_sinCos_S * rotorAngle)
{
    float32_t amps = ZERO;
    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (IO_bridge_getPhaseCurrent(this->config->bridge, (IO_bridge_phase_E)phase, &amps))
        {
            this->phaseCurrent_a[phase] = amps;
        }
    }

    lib_transforms_clarke(
        this->phaseCurrent_a[IO_BRIDGE_PHASE_U],
        this->phaseCurrent_a[IO_BRIDGE_PHASE_V],
        this->phaseCurrent_a[IO_BRIDGE_PHASE_W],
        &this->iAlpha_a,
        &this->iBeta_a
    );
    lib_transforms_park(this->iAlpha_a, this->iBeta_a, *rotorAngle, &this->iD_a, &this->iQ_a);

    this->rotorTorque_Nm = this->isAligned ? (this->iQ_a * this->config->foc.torqueConstant_NmPerA) : ZERO;
    const float32_t knownCogging_Nm = this->config->foc.coggingKff * this->coggingTorque_Nm;
    app_rotorEstimation_setTorqueInput(this->config->rotorEstimator, (this->rotorTorque_Nm + knownCogging_Nm));
}

// Integrals zero at a method's first step: a method runs from an enable to the next
// disable. [impl->fw~mc_032~1] the position zero until that step.
void Motor::resetOnEntry(bool isDriving, bool isFieldOriented)
{
    if ((!isDriving) || (!isFieldOriented))
    {
        this->currentPi[AxisD].reset();
        this->currentPi[AxisQ].reset();
        this->velocityPi.reset();
        this->positionPi.reset();
    }
    if (!isDriving)
    {
        this->rotorTurns = 0;
        this->rotorPositionZero_rad = this->rotorAngle_rad;
        this->rotorPosition_rad = ZERO;
    }
}

// [impl->fw~mc_011~1] the sector pattern for the electrical angle advanced by the
// lead, at a duty proportional to the target magnitude, clamped to max.
void Motor::stepSixStep(const lib_transforms_sinCos_S * rotorAngle)
{
    (void)rotorAngle;
    const float32_t lead_rad = (this->velocitySetpoint_radPerSec >= ZERO) ? RAD_90DEG : -RAD_90DEG;
    const float32_t target = lib_transforms_wrapAngle(this->magneticAngle_rad + lead_rad);
    this->magneticAngleTarget_rad = target;

    // TODO - compute duty cycle from velocity error
    const float32_t duty01 = MIN_OF(fabsf(this->velocitySetpoint_radPerSec) / this->config->maxVelocity_radPerSec, APP_MOTORCONTROL_MAX_DUTY_01);

    // A two-phase pattern's field lies mid-sector (30, 90, .. 330 deg): the sector
    // holding the target applies the nearest field, within 30 deg of the lead.
    if (target < RAD_60DEG)
    {
        this->setPattern(duty01, ZERO, ZERO, true, false, true);   // U+ W-  (30 deg)
    }
    else if (target < RAD_120DEG)
    {
        this->setPattern(ZERO, duty01, ZERO, false, true, true);   // V+ W-  (90 deg)
    }
    else if (target < RAD_180DEG)
    {
        this->setPattern(ZERO, duty01, ZERO, true, true, false);   // V+ U- (150 deg)
    }
    else if (target < RAD_240DEG)
    {
        this->setPattern(ZERO, ZERO, duty01, true, false, true);   // W+ U- (210 deg)
    }
    else if (target < RAD_300DEG)
    {
        this->setPattern(ZERO, ZERO, duty01, false, true, true);   // W+ V- (270 deg)
    }
    else // angle < 360
    {
        this->setPattern(duty01, ZERO, ZERO, true, true, false);   // U+ V- (330 deg)
    }
}

// [impl->fw~mc_020~1] (0, V) in the rotor frame, V the speed target's share of V_max
void Motor::stepSinusoidal(const lib_transforms_sinCos_S * rotorAngle)
{
    const float32_t velocityShare = MIN_OF(fabsf(this->velocitySetpoint_radPerSec) / this->config->maxVelocity_radPerSec, 1.0f);
    const float32_t vQ = SGN(this->velocitySetpoint_radPerSec) * velocityShare * this->config->maxVoltage_V;
    lib_transforms_inversePark(ZERO, vQ, *rotorAngle, &this->vAlpha_v, &this->vBeta_v);
    this->setEnables(true, true, true);
}

// [impl->fw~mc_022~1] i_q* = T_t / K_t, in the torque loop
void Motor::stepFocTorque(const lib_transforms_sinCos_S * rotorAngle)
{
    (void) this->focTorqueLoop(this->torqueSetpoint_Nm, rotorAngle);
}

void Motor::stepFocVelocity(const lib_transforms_sinCos_S * rotorAngle)
{
    (void) this->focVelocityLoop(this->velocitySetpoint_radPerSec, rotorAngle);
}

void Motor::stepFocPosition(const lib_transforms_sinCos_S * rotorAngle)
{
    (void) this->focPositionLoop(this->cyclePositionTarget_rad, this->cycleProfileVelocity_radPerSec, rotorAngle);
}

void Motor::applyOutputs(bool isVectorOutput, bool isBridgeOn)
{
    if (isVectorOutput)
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

    // Written on change only, so a break's hardware clear stands until the latch.
    if (isBridgeOn != this->bridgeEnabled)
    {
        IO_bridge_setOutputEnabled(this->config->bridge, isBridgeOn);
        this->bridgeEnabled = isBridgeOn;
    }
}

bool Motor::focTorqueLoop(float32_t targetTorque_Nm, const lib_transforms_sinCos_S * rotorAngle)
{
    const float32_t targetDCurrent_a = 0.0f;
    // [impl->fw~mc_028~1] the q-axis torque command: the method's demand less
    // the compensated cogging, clamped to the current limit
    const float32_t qTorque_Nm = app_motorControl_private_saturate(
        targetTorque_Nm - (this->config->foc.coggingKff * this->coggingTorque_Nm), this->torqueLimit_Nm);
    const float32_t targetQCurrent_a = qTorque_Nm * this->inverseTorqueConstant_APerNm;

    // [impl->fw~mc_023~1]
    const float32_t errorDCurrent_a = targetDCurrent_a - this->iD_a;
    float32_t vD = this->currentPi[AxisD].step(errorDCurrent_a);

    const float32_t errorQCurrent_a = targetQCurrent_a - this->iQ_a;
    float32_t vQ = this->currentPi[AxisQ].step(errorQCurrent_a);

    // [impl->fw~mc_024~1]
    const float32_t vLim = MIN_OF(this->config->maxVoltage_V, (this->busVoltage * LIB_TRANSFORMS_1_DIV_SQRT3));
    const float32_t vVectorMag = sqrtf((vD*vD) + (vQ*vQ));
    const bool isLimited = (vVectorMag >= (vLim * (1.0f - APP_MOTORCONTROL_VOLTAGE_LIMIT_ROUNDING)));
    if (vVectorMag > vLim)
    {
        const float32_t k = vLim / vVectorMag; // limit / mag
        vD *= k;
        vQ *= k;

        this->currentPi[AxisD].backCalculate(errorDCurrent_a, vD);
        this->currentPi[AxisQ].backCalculate(errorQCurrent_a, vQ);
    }

    lib_transforms_inversePark(vD, vQ, *rotorAngle, &this->vAlpha_v, &this->vBeta_v);

    this->setEnables(true, true, true);
    return isLimited;
}

// [impl->fw~mc_025~1]
bool Motor::focVelocityLoop(float32_t targetVelocity_radPerSec, const lib_transforms_sinCos_S * rotorAngle)
{
    const float32_t velocityError_radPerSec = targetVelocity_radPerSec - this->velocityMeasured_radPerSec;
    const float32_t disturbanceFeedForward_Nm = this->unmodeledTorque_Nm * this->config->foc.velocityKff;
    float32_t targetTorque_Nm = this->velocityPi.step(velocityError_radPerSec) - disturbanceFeedForward_Nm;

    const bool isClamped = (fabsf(targetTorque_Nm) > this->torqueLimit_Nm);
    if (isClamped)
    {
        targetTorque_Nm = SGN(targetTorque_Nm) * this->torqueLimit_Nm;

        // the integral is solved with the feed-forward still in the demand
        this->velocityPi.backCalculate(velocityError_radPerSec, (targetTorque_Nm + disturbanceFeedForward_Nm));
    }

    const bool isLimited = this->focTorqueLoop(targetTorque_Nm, rotorAngle);
    // the integral held while the voltage limit holds back a demand it pushes on
    if (isLimited && (!isClamped) && (SGN(velocityError_radPerSec) == SGN(targetTorque_Nm)))
    {
        this->velocityPi.hold(velocityError_radPerSec);
    }
    return isLimited;
}

// [impl->fw~mc_029~1] w_c = K_pp e + K_pi S_p + w_f, clamped to +/-w_max with the
// integral held through the clamp: back-calculated against a w_f at w_max, S_p
// would wind against the proportional term.
bool Motor::focPositionLoop(float32_t positionTarget_rad, float32_t profileVelocity_radPerSec, const lib_transforms_sinCos_S * rotorAngle)
{
    const float32_t positionError_rad = positionTarget_rad - this->rotorPosition_rad;
    float32_t velocityCommand_radPerSec = this->positionPi.step(positionError_rad) + profileVelocity_radPerSec;

    const bool isClamped = (fabsf(velocityCommand_radPerSec) > this->config->maxVelocity_radPerSec);
    if (isClamped)
    {
        velocityCommand_radPerSec = SGN(velocityCommand_radPerSec) * this->config->maxVelocity_radPerSec;
        this->positionPi.hold(positionError_rad);
    }
    const bool isLimited = this->focVelocityLoop(velocityCommand_radPerSec, rotorAngle);
    if (isLimited && (!isClamped) && (SGN(positionError_rad) == SGN(velocityCommand_radPerSec)))
    {
        this->positionPi.hold(positionError_rad);
    }
    return isLimited;
}

// The phase outputs are written by run1ms (task) while Off or Aligning, by run50us
// (ISR) while Driving; never both in one state. A half-written static pattern lasts one cycle.
void Motor::setEnables(bool enableU, bool enableV, bool enableW)
{
    this->enable[IO_BRIDGE_PHASE_U] = enableU;
    this->enable[IO_BRIDGE_PHASE_V] = enableV;
    this->enable[IO_BRIDGE_PHASE_W] = enableW;
}

void Motor::setPattern(float32_t dutyU, float32_t dutyV, float32_t dutyW, bool enableU, bool enableV, bool enableW)
{
    this->duty[IO_BRIDGE_PHASE_U] = dutyU;
    this->duty[IO_BRIDGE_PHASE_V] = dutyV;
    this->duty[IO_BRIDGE_PHASE_W] = dutyW;
    this->setEnables(enableU, enableV, enableW);
}


/* Private Class Function Definitions: the 1 ms pass */

void Motor::updateFaultLatches()
{
    // [impl->fw~safety_002~1] a persistently invalid encoder: a stale position
    // would commutate the live bridge wrong
    IO_AS5048_status_E encoderStatus = IO_AS5048_STATUS_IDLE;
    (void)IO_AS5048_getStatus(this->config->encoder, &encoderStatus);
    if (encoderStatus == IO_AS5048_STATUS_FAULT)
    {
        this->encoderFaultCount++;
        if (this->encoderFaultCount > this->config->encoderFaultLimit)
        {
            this->latchFault();
        }
    }
    else
    {
        this->encoderFaultCount = 0U;
    }

    this->updateOvercurrentLatch();

    // [impl->fw~safety_003~1] commanded on either side of a read that says off:
    // a break. The second look rules out a disable landing between the two.
    const bool wasCommanded = this->bridgeEnabled;
    bool outputEnabled = true;
    if (wasCommanded &&
        IO_bridge_getOutputEnabled(this->config->bridge, &outputEnabled) &&
        (!outputEnabled) &&
        this->bridgeEnabled)
    {
        this->latchFault();
    }
}

// [impl->fw~safety_001~1] any phase-current magnitude above the phase trip or a
// DC-bus current above the bus trip latches a fault.
void Motor::updateOvercurrentLatch()
{
    float32_t amps = ZERO;

    for (size_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
    {
        if (fabsf(this->phaseCurrent_a[phase]) > this->config->overcurrentPhaseTrip_A)
        {
            this->latchFault();
        }
    }

    if (IO_bridge_getBusCurrent(this->config->bridge, &amps))
    {
        this->busCurrent = amps;
        if (fabsf(amps) > this->config->overcurrentBusTrip_A)
        {
            this->latchFault();
        }
    }
}

// [impl->fw~ops_002~1] a latch counts once, so a reader that misses the
// latched state between its passes still sees the count move.
// [impl->fw~safety_001~1] the latch withdraws the enable request: a clear leaves the bridge dark.
void Motor::latchFault()
{
    if (!this->faultLatched)
    {
        this->faultLatched = true;
        this->faultLatches++;
    }
    this->modeRequested = APP_MOTORCONTROL_MODE_OFF;
}

// [impl->fw~mc_033~1] the request, except another method while one drives: the
// method a drive runs stays until it is disabled
app_motorControl_mode_E Motor::decideMode(DriveState lastState) const
{
    const app_motorControl_mode_E requested = this->modeRequested;
    const bool isMethodChangeWhileDriving = ((lastState == DriveState::Driving) &&
                                             (requested != APP_MOTORCONTROL_MODE_OFF) &&
                                             (requested != this->modeCurrent));
    return isMethodChangeWhileDriving ? this->modeCurrent : requested;
}

Motor::DriveState Motor::decideDriveState(DriveState lastState, app_motorControl_mode_E mode)
{
    // [impl->fw~mc_006~1]
    this->isDriveAllowed =
        (dev_gateDriver_isOperational(this->config->gateDriver)) &&
        (!this->faultLatched);

    DriveState state = DriveState::Off;
    if ((!this->isDriveAllowed) || (mode == APP_MOTORCONTROL_MODE_OFF))
    {
        state = DriveState::Off;
    }
    else if ((lastState == DriveState::Off) || (lastState == DriveState::Precharging))
    {
        // [impl->fw~mc_021~1] every enable from dark charges the bootstraps through the
        // low sides: a high side commanded on from an empty one trips VDS protection.
        const bool charged =
            (lib_timer_runTimerWithEnable(&this->prechargeDwell, true) == LIB_TIMER_STATE_EXPIRED);
        state = charged ? (this->isAligned ? DriveState::Driving : DriveState::Aligning)
                        : DriveState::Precharging;
    }
    else
    {
        state = this->isAligned ? DriveState::Driving : DriveState::Aligning;
    }
    return state;
}

// [impl->fw~mc_027~1] the targets in effect: zero while dark, seeded at the enable
// (speed from the rotor estimate, torque from zero), then moved toward the written
// values at their rate limits every pass.
void Motor::slewTargets(DriveState lastState, DriveState state, float32_t estimatedVelocity_radPerSec)
{
    if (state == DriveState::Off)
    {
        this->velocitySetpoint_radPerSec = ZERO;
        this->torqueSetpoint_Nm = ZERO;
        if (lastState != DriveState::Off)
        {
            // [impl->fw~mc_026~1] [impl->fw~mc_030~1] a disable or fault drops the
            // torque request and the position setpoint once; a write while dark
            // is held to the enable
            this->torqueSetpointRequested_Nm = ZERO;
            this->torqueRateLimit_NmPerSec = ZERO;
            this->positionSetpoint = PositionSetpoint{};
            this->positionSetpointPending = false;
            this->lastWrite = LastWrite::None;
            this->lastWriteApplied = LastWrite::None;
        }
    }
    else if (lastState == DriveState::Off)
    {
        this->velocitySetpoint_radPerSec = estimatedVelocity_radPerSec;
        this->torqueSetpoint_Nm = ZERO;
    }
    else
    {
        // [impl->fw~mc_027~1] a jog that follows a position move starts from the
        // profile's velocity, not a speed target that stood behind the move
        const LastWrite written = this->lastWrite;
        if ((written == LastWrite::Speed) && (this->lastWriteApplied != LastWrite::Speed))
        {
            this->velocitySetpoint_radPerSec = this->profileVelocity_radPerSec;
        }
        this->lastWriteApplied = written;
        this->velocitySetpoint_radPerSec = app_motorControl_private_slewToward(
            this->velocitySetpoint_radPerSec, this->velocitySetpointRequested_radPerSec,
            this->velocityRateLimit_radPerSec2, APP_MOTORCONTROL_RUN1MS_PERIOD_S);
        this->torqueSetpoint_Nm = app_motorControl_private_slewToward(
            this->torqueSetpoint_Nm, this->torqueSetpointRequested_Nm,
            this->torqueRateLimit_NmPerSec, APP_MOTORCONTROL_RUN1MS_PERIOD_S);
    }
}

// [impl->fw~mc_031~1] theta* and w_f zero until the position method's first step
// after the enable; the count tells the ISR a pass has published them.
void Motor::updateMoveProfile(DriveState state, app_motorControl_mode_E mode)
{
    if ((state == DriveState::Driving) && methods[mode].usesProfile)
    {
        this->runMoveProfile();
    }
    else
    {
        this->positionTarget_rad = ZERO;
        this->profileVelocity_radPerSec = ZERO;
        this->profileArrived = false;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    this->profilePasses = this->profilePasses + 1U;
}

// [impl->fw~mc_031~1] theta* advances by w_f each pass, saturated to +/-theta_max; w_f
// from whichever of the position setpoint and the speed target was written last:
// the minimum-time profile to theta_s arriving at w_s, or the jog.
void Motor::runMoveProfile()
{
    float32_t velocity_radPerSec = ZERO;

    if (this->positionSetpointPending)
    {
        this->positionSetpointPending = false;
        this->profileArrived = false;
    }

    switch (this->lastWrite)
    {
        case LastWrite::Speed:
            velocity_radPerSec = this->velocitySetpoint_radPerSec;
            break;

        case LastWrite::Position:
            velocity_radPerSec = this->planPositionMove();
            break;

        default:
        case LastWrite::None:
            break;
    }

    // w_f zero while it would push theta* past a bound
    float32_t next_rad = this->positionTarget_rad + (velocity_radPerSec * APP_MOTORCONTROL_RUN1MS_PERIOD_S);
    const float32_t bound_rad = this->config->foc.maxPosition_rad;
    if (next_rad > bound_rad)
    {
        next_rad = bound_rad;
        velocity_radPerSec = ZERO;
    }
    else if (next_rad < -bound_rad)
    {
        next_rad = -bound_rad;
        velocity_radPerSec = ZERO;
    }
    this->positionTarget_rad = next_rad;
    this->profileVelocity_radPerSec = velocity_radPerSec;
}

// w_f toward the position setpoint: toward v, braking at a to reach w_s on theta_s,
// then held at w_s past it. On arrival at rest theta* lands on theta_s.
float32_t Motor::planPositionMove()
{
    const float32_t period_s = APP_MOTORCONTROL_RUN1MS_PERIOD_S;
    float32_t velocity_radPerSec = this->profileHeldVelocity_radPerSec;
    if (!this->profileArrived)
    {
        const PositionSetpoint sp = this->positionSetpoint;
        const float32_t distance_rad = sp.position_rad - this->positionTarget_rad;
        const float32_t direction = (distance_rad < ZERO) ? -1.0f : 1.0f;
        const float32_t remaining_rad = fabsf(distance_rad);
        if (sp.accelLimit_radPerSec2 > ZERO)
        {
            // The speed from which braking at a, one a T step per pass, reaches w_s
            // exactly at theta_s; w_f moves toward it, or toward v, at a per second.
            const float32_t accelStep_radPerSec = sp.accelLimit_radPerSec2 * period_s;
            const float32_t halfAccelStep_radPerSec = 0.5f * accelStep_radPerSec;
            const float32_t pointVelocity_radPerSec = sp.velocityAtPoint_radPerSec;
            const float32_t radicand = MAX_OF(ZERO, (halfAccelStep_radPerSec * halfAccelStep_radPerSec) + (pointVelocity_radPerSec * pointVelocity_radPerSec)
                                                    - (accelStep_radPerSec * pointVelocity_radPerSec) + (2.0f * sp.accelLimit_radPerSec2 * remaining_rad));
            const float32_t speed_radPerSec = MIN_OF(sp.velocityLimit_radPerSec, sqrtf(radicand) - halfAccelStep_radPerSec);
            velocity_radPerSec = app_motorControl_private_slewToward(this->profileVelocity_radPerSec, direction * speed_radPerSec, sp.accelLimit_radPerSec2, period_s);
        }
        else
        {
            velocity_radPerSec = direction * sp.velocityLimit_radPerSec;
        }

        // this pass reaches theta_s while closing on it: on it at rest, or past it at w_s
        if (((velocity_radPerSec * direction) >= ZERO) && (remaining_rad <= (fabsf(velocity_radPerSec) * period_s)))
        {
            this->profileArrived = true;
            this->profileHeldVelocity_radPerSec = direction * sp.velocityAtPoint_radPerSec;
            velocity_radPerSec = this->profileHeldVelocity_radPerSec;
            if (sp.velocityAtPoint_radPerSec == ZERO)
            {
                this->positionTarget_rad = sp.position_rad;
            }
        }
    }
    return velocity_radPerSec;
}

void Motor::applyStateOutputs(DriveState state, float32_t estimatedAngle_rad)
{
    switch (state)
    {
        case DriveState::Off:
            this->setPattern(ZERO, ZERO, ZERO, false, false, false);
            lib_timer_stopTimer(&this->alignmentDwell);
            lib_timer_stopTimer(&this->prechargeDwell);
            break;

        case DriveState::Precharging:
            this->setPattern(ZERO, ZERO, ZERO, true, true, true);
            break;

        case DriveState::Aligning:
            // [impl->fw~mc_012~1] a stator field on the phase-U axis (U sourcing, V and W
            // returning) parks the rotor for the dwell: the estimate there is electrical zero
            this->setPattern(this->config->alignmentDuty01, ZERO, ZERO, true, true, true);
            if (lib_timer_runTimerWithEnable(&this->alignmentDwell, true) == LIB_TIMER_STATE_EXPIRED)
            {
                this->alignmentOffset_rad = estimatedAngle_rad;
                this->isAligned = true;
            }
            break;

        case DriveState::Driving:
            // The active method's step owns the outputs (run50us).
            break;
    }
}


/* Public Class Function Definitions */

bool Motor::init(const app_motorControl_channelConfig_S * channelConfig)
{
    static_assert(COUNTOF(Motor::methods) == APP_MOTORCONTROL_MODE_COUNT, "a method row per mode");
    bool isValid = true;

    isValid &= (channelConfig->gateDriver < DEV_GATEDRIVER_CHANNEL_COUNT);
    isValid &= (channelConfig->bridge < IO_BRIDGE_CHANNEL_COUNT);
    isValid &= (channelConfig->rotorEstimator < APP_ROTORESTIMATION_CHANNEL_COUNT);
    isValid &= (channelConfig->encoder < IO_AS5048_CHANNEL_COUNT);
    // Divisors and multipliers below: a zero is rejected here, not defended at each use.
    isValid &= (channelConfig->motorPolePairs > 0U);
    isValid &= (channelConfig->maxVelocity_radPerSec > ZERO);
    isValid &= (channelConfig->pwmPeriod_s > ZERO);
    isValid &= (channelConfig->maxVoltage_V > ZERO);
    isValid &= (channelConfig->defaultMode > APP_MOTORCONTROL_MODE_OFF);
    isValid &= (channelConfig->defaultMode < APP_MOTORCONTROL_MODE_COUNT);
    isValid &= (channelConfig->prechargeTime_ms > 0U);
    // [impl->fw~mc_012~1] an alignment duty in (0, the six-step maximum], a dwell
    isValid &= ((channelConfig->alignmentDuty01 > ZERO) && (channelConfig->alignmentDuty01 <= APP_MOTORCONTROL_MAX_DUTY_01));
    isValid &= (channelConfig->alignmentDwell_ms > 0U);
    // [impl->fw~safety_001~1] the trips above zero
    isValid &= (channelConfig->overcurrentPhaseTrip_A > ZERO);
    isValid &= (channelConfig->overcurrentBusTrip_A > ZERO);
    // [impl->fw~mc_022~1] I_max under the phase trip
    isValid &= (channelConfig->foc.maxCurrent_A > ZERO);
    isValid &= (channelConfig->foc.maxCurrent_A < channelConfig->overcurrentPhaseTrip_A);
    // [impl->fw~mc_023~1] R, L, w_bw nonzero, w_bw T bounded: K_p, K_i follow
    isValid &= (channelConfig->foc.motorR_ohm > ZERO);
    isValid &= (channelConfig->foc.motorL_H > ZERO);
    isValid &= (channelConfig->foc.currentLoopBandwidth_radPerSec > ZERO);
    isValid &= ((channelConfig->foc.currentLoopBandwidth_radPerSec * channelConfig->pwmPeriod_s) <= APP_MOTORCONTROL_FOC_MAX_BANDWIDTH_PERIOD_PRODUCT);
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
        PiConfig piConfig =
        {
            .kp = channelConfig->foc.motorL_H * channelConfig->foc.currentLoopBandwidth_radPerSec,
            .ki = channelConfig->foc.motorR_ohm * channelConfig->foc.currentLoopBandwidth_radPerSec,
            .period_s = channelConfig->pwmPeriod_s,
        };
        for (size_t axis = 0U; axis < AxisCount; axis++)
        {
            isValid &= this->currentPi[axis].init(piConfig);
        }

        piConfig.kp = channelConfig->foc.velocityKp_NmPerRadPerSec;
        piConfig.ki = channelConfig->foc.velocityKi_NmPerRad;
        isValid &= this->velocityPi.init(piConfig);

        piConfig.kp = channelConfig->foc.positionKp_perSec;
        piConfig.ki = channelConfig->foc.positionKi_perSec2;
        isValid &= this->positionPi.init(piConfig);
    }

    if (isValid)
    {
        this->config = channelConfig;
        this->torqueLimit_Nm = channelConfig->foc.torqueConstant_NmPerA * channelConfig->foc.maxCurrent_A;
        this->inverseTorqueConstant_APerNm = 1.0f / channelConfig->foc.torqueConstant_NmPerA;

        this->modeCurrent = APP_MOTORCONTROL_MODE_OFF;
        this->modeRequested = APP_MOTORCONTROL_MODE_OFF;
        this->modeSelected = channelConfig->defaultMode;
        this->driveState = DriveState::Off;

        this->isAligned = false;
        this->alignmentOffset_rad = ZERO;
        lib_timer_init(&this->alignmentDwell, LIB_TIMER_PRECISION_MS, channelConfig->alignmentDwell_ms);
        lib_timer_init(&this->prechargeDwell, LIB_TIMER_PRECISION_MS, channelConfig->prechargeTime_ms);

        this->faultLatched = false;
        this->faultLatches = 0U;
        (void) IO_bridge_setOutputEnabled(channelConfig->bridge, false);
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
        this->lastWriteApplied = LastWrite::None;
        this->positionTarget_rad = ZERO;
        this->profileVelocity_radPerSec = ZERO;
        this->profileArrived = false;
        this->profileHeldVelocity_radPerSec = ZERO;
        this->profilePasses = 0U;
        this->profilePassesSeen = 0U;
        this->cyclesSincePass = 0U;
        this->cycleTargetFrom_rad = ZERO;
        this->cycleTargetTo_rad = ZERO;
        this->cycleProfileVelocity_radPerSec = ZERO;
        this->cyclePositionTarget_rad = ZERO;
        this->pwmPeriodPerPass = channelConfig->pwmPeriod_s / APP_MOTORCONTROL_RUN1MS_PERIOD_S;
    }
    return isValid;
}

void Motor::run1ms()
{
    if (this->config != NULL)
    {
        // The rotor estimate as it stands this pass, for the enable seed and the
        // alignment capture; the ISR owns the estimate members.
        float32_t estimatedAngle_rad = ZERO;
        float32_t estimatedVelocity_radPerSec = ZERO;
        float32_t unmodeledTorque_Nm = ZERO;
        app_rotorEstimation_getState(this->config->rotorEstimator, &estimatedAngle_rad, &estimatedVelocity_radPerSec, &unmodeledTorque_Nm);

        this->updateFaultLatches();

        float32_t busVoltage = ZERO;
        if (IO_bridge_getBusVoltage(this->config->bridge, &busVoltage))
        {
            this->busVoltage = busVoltage;
        }

        // The request takes effect this pass; the state before the mode, so the ISR
        // sees a disable as Off, never a stale method driving.
        const DriveState lastState = this->driveState;
        const app_motorControl_mode_E mode = this->decideMode(lastState);
        const DriveState state = this->decideDriveState(lastState, mode);
        this->driveState = state;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        this->modeCurrent = mode;

        this->slewTargets(lastState, state, estimatedVelocity_radPerSec);
        this->updateMoveProfile(state, mode);
        this->applyStateOutputs(state, estimatedAngle_rad);
    }
}

// [impl->fw~mc_015~1] the PWM-synchronous step, in the bridge callback's ISR
// context: the active method commutates from the inputs run1ms published and
// the cycle's phase currents, then the bridge takes the outputs.
void Motor::run50us(IO_bridge_channel_E bridge)
{
    if ((this->config != NULL) && (this->config->bridge == bridge))
    {
        const DriveState state = this->driveState;
        const Method & method = methods[this->modeCurrent];
        const bool isDriving = (state == DriveState::Driving);
        const bool isFieldOrientedDriving = (isDriving && method.isFieldOriented);

        this->updateRotorPosition();
        if (isFieldOrientedDriving)
        {
            this->updateCoggingTorque();
        }
        else
        {
            this->coggingTorque_Nm = ZERO;
        }
        this->updateCyclePositionTarget();

        // One sine/cosine pair of the rotor angle serves every transform this cycle.
        lib_transforms_sinCos_S rotorAngle = { .sin = ZERO, .cos = 1.0f };
        (void) HW_CORDIC_sinCos(this->magneticAngle_rad, &rotorAngle);

        this->measureCurrents(&rotorAngle);
        this->resetOnEntry(isDriving, method.isFieldOriented);
        if (isDriving && (method.step != nullptr))
        {
            (this->*method.step)(&rotorAngle);
        }
        this->applyOutputs((isDriving && method.isVectorOutput),
                           (state != DriveState::Off));
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
        snapshot->rotorPosition_rad = this->rotorPosition_rad;
        snapshot->faultLatches      = this->faultLatches;

        // Coarse state the ring reads for fw~mc_009 (the ring carries that impl
        // tag): fault wins, else the live bridge-enable tells driving from idle.
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
        ratings->maxTorque_Nm           = this->torqueLimit_Nm;
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
            const float32_t torquePerSpeed_NmPerRadPerSec = this->torqueLimit_Nm / this->config->maxVelocity_radPerSec;
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
        this->torqueRateLimit_NmPerSec = app_motorControl_private_clampRateLimit(rateLimit_NmPerSec);
        std::atomic_signal_fence(std::memory_order_seq_cst);   // limit before value, as setVelocity
        this->torqueSetpointRequested_Nm = app_motorControl_private_saturate(torque_Nm, this->torqueLimit_Nm);
    }
}

// [impl->fw~mc_033~1] a method request selects and enables it, except for another
// method while one drives; OFF disables and keeps the selection.
void Motor::setMode(app_motorControl_mode_E mode)
{
    if ((this->config != NULL) && (mode < APP_MOTORCONTROL_MODE_COUNT))
    {
        const bool isMethodChangeWhileDriving = ((this->driveState == DriveState::Driving) &&
                                                 (mode != APP_MOTORCONTROL_MODE_OFF) &&
                                                 (mode != this->modeCurrent));
        if (!isMethodChangeWhileDriving)
        {
            this->modeRequested = mode;
            if (mode != APP_MOTORCONTROL_MODE_OFF)
            {
                this->modeSelected = mode;
            }
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

// [impl->fw~mc_015~1] each channel steps on its own bridge's callback
void app_motorControl_run50us(IO_bridge_channel_E bridge)
{
    for (size_t channel = 0U; channel < APP_MOTORCONTROL_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run50us(bridge);
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
