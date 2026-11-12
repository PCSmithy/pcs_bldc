#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_bridge.h"
#include "IO_AS5048.h"

#include "dev_gateDriver.h"

#include "app_rotorEstimation.h"
#include "app_motorControl_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */


/* Typedefs */

typedef enum
{
    APP_MOTORCONTROL_MODE_OFF,
    APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
    APP_MOTORCONTROL_MODE_SINUSOIDAL,
    APP_MOTORCONTROL_MODE_FOC_TORQUE,
    APP_MOTORCONTROL_MODE_FOC_VELOCITY,
    APP_MOTORCONTROL_MODE_FOC_POSITION,
    APP_MOTORCONTROL_MODE_COUNT,
} app_motorControl_mode_E; // TODO - convert this to `enum class` if/when consumers are C++

// Coarse bridge state for the operator display (fw~mc_009): DISABLED (bridge
// dark), ENABLED (bridge driving), or FAULTED (a latched fault holds it off).
typedef enum
{
    APP_MOTORCONTROL_STATE_DISABLED,
    APP_MOTORCONTROL_STATE_ENABLED,
    APP_MOTORCONTROL_STATE_FAULTED,
    APP_MOTORCONTROL_STATE_COUNT,
} app_motorControl_state_E;

#define APP_MOTORCONTROL_COG_HARMONICS (2U)

// One harmonic of the cogging model T_cog = sum A sin(n theta + phi) on the raw
// mechanical angle, positive with increasing angle (fw~mc_028)
typedef struct
{
    uint16_t  order_perRev;   // n, cycles per mechanical revolution
    float32_t amplitude_Nm;   // A, >= 0; zero is no harmonic
    float32_t phase_rad;      // phi
} app_motorControl_cogHarmonic_S;

typedef struct
{
    float32_t maxCurrent_A;  // I_max: the FOC Q-axis current a full-scale target commands (fw~mc_022)
    float32_t motorL_H;
    float32_t motorR_ohm;
    float32_t torqueConstant_NmPerA;     // K_t = 1.5 * K_e for a sinusoidal motor, K_e the per-phase back-EMF constant in V/(rad/s)
    float32_t velocityKp_NmPerRadPerSec; // K_vp: velocity-loop bandwidth K_vp / J, kept under the estimator's poles (fw~mc_025)
    float32_t velocityKi_NmPerRad;       // K_vi: the integral corner K_vi / K_vp, well under that bandwidth
    float32_t currentLoopBandwidth_radPerSec;
    float32_t velocityKff;               // k_ff in [0, 1]: the share of the estimator's T_d fed forward against (fw~mc_025)
    app_motorControl_cogHarmonic_S cogging[APP_MOTORCONTROL_COG_HARMONICS];
    float32_t coggingKff;                // k_cog in [0, 1]: the share of T_cog compensated (fw~mc_028)
    float32_t positionKp_perSec;         // K_pp: rad/s of velocity command per rad of position error (fw~mc_029)
    float32_t positionKi_perSec2;        // K_pi: per rad.s of integrated error
    float32_t maxPosition_rad;           // the travel bound either side of the enable point (fw~mc_031)
} app_motorControl_focConfig_S;

typedef struct
{
    dev_gateDriver_channel_E gateDriver;
    IO_bridge_channel_E bridge;

    float32_t maxVelocity_radPerSec;

    app_rotorEstimation_channel_E rotorEstimator;   // the rotor angle + velocity estimate (fw~est_rotor_002)
    IO_AS5048_channel_E encoder;   // its encoder, read here only for the fault status (fw~safety_002)

    uint8_t motorPolePairs;
    float32_t pwmPeriod_s;   // the bridge's PWM period: the step's time base
    float32_t maxVoltage_V;  // V_max: the voltage a full-scale target commands (fw~mc_020)

    app_motorControl_focConfig_S foc;

    app_motorControl_mode_E defaultMode;   // the method selected at power-up; never OFF
    uint32_t prechargeTime_ms;   // low sides on before any enable drives: the bootstrap charge (fw~mc_021)
    float32_t alignmentDuty01;   // phase U's duty in the alignment pattern, at most the six-step maximum (fw~mc_012)
    uint32_t alignmentDwell_ms;  // how long the pattern holds before the offset capture

    float32_t overcurrentPhaseTrip_A;   // a phase-current magnitude above it latches a fault (fw~safety_001)
    float32_t overcurrentBusTrip_A;     // a DC-bus current above it latches a fault
    uint16_t encoderFaultLimit;         // more consecutive invalid encoder reads latch a fault (fw~safety_002)
} app_motorControl_channelConfig_S;

typedef struct
{
    const app_motorControl_channelConfig_S * channels;
    size_t numChannels;
} app_motorControl_config_S;

// Telemetry/diagnostic view of one channel's live state, and the source the
// operator display (app_rgbLedRing) reads for state + speedometer indication.
typedef struct
{
    app_motorControl_mode_E  mode;
    app_motorControl_state_E state;                        // disabled / enabled / faulted
    bool                     isAligned;
    float32_t                magneticAngle_rad;            // rotor electrical angle
    float32_t                velocitySetpoint_radPerSec;   // the speed target in effect (fw~mc_027)
    float32_t                torqueTarget_Nm;              // the torque target in effect (fw~mc_026, fw~mc_027)
    float32_t                velocityMeasured_radPerSec;   // the rotor estimator's velocity (fw~est_rotor_002)
    float32_t                positionTarget_rad;           // theta*, the position target in effect (fw~mc_031)
    float32_t                rotorPosition_rad;            // the multi-turn rotor position, zero at the enable (fw~mc_032)
    app_motorControl_mode_E  selectedMode;                 // the method an enable runs; OFF leaves it selected
    uint16_t                 faultLatches;                 // fault latches since init, each counted once
} app_motorControl_snapshot_S;

// The channel's configured ratings, the bounds a host holds its setpoints to
// (fw~conn_server_008).
typedef struct
{
    float32_t torqueConstant_NmPerA;
    float32_t maxCurrent_A;
    float32_t maxTorque_Nm;          // K_t * I_max
    float32_t maxVelocity_radPerSec;
    float32_t maxPosition_rad;
} app_motorControl_ratings_S;

/* Public Function Declarations */

bool app_motorControl_init(const app_motorControl_config_S * const config);

// Copy out the channel's live mode/alignment/electrical-angle state. Returns
// false for an out-of-range channel, a NULL destination, or an uninitialized
// module, leaving the destination unchanged.
bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot);

// Copy out the channel's ratings; false for an out-of-range channel, a NULL
// destination, or an uninitialized module.
bool app_motorControl_getRatings(app_motorControl_channel_E channel, app_motorControl_ratings_S * const ratings);

void app_motorControl_run1ms(void);
// The PWM-synchronous step of every channel on `bridge`, from that bridge's cycle callback.
void app_motorControl_run50us(IO_bridge_channel_E bridge);

// Write a target with its rate limit in the target's unit per second; zero
// steps it to the value on the next 1 ms pass (fw~mc_027). A speed target
// while the torque method is selected also writes the torque target (fw~mc_026).
void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2);
void app_motorControl_setTorque(app_motorControl_channel_E channel, float32_t torque_Nm, float32_t rateLimit_NmPerSec);
// Write the position setpoint (fw~mc_030): the position, its velocity and acceleration
// limits (zero: the maximum velocity, a step) and the velocity at the point. The move
// profile (fw~mc_031) drives the position target to it; a disable drops it.
void app_motorControl_setPositionSetpoint(app_motorControl_channel_E channel, float32_t position_rad, float32_t velocityLimit_radPerSec,
                                          float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec);
// A method request selects and enables it, ignored for another method while one
// drives; OFF disables and keeps the selection (fw~mc_033).
void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode);

// The fault-clear action (fw~safety_001..003): releases the latch; the bridge stays
// dark until a new enable request. A no-op for an out-of-range channel or an
// uninitialized module.
void app_motorControl_clearFault(app_motorControl_channel_E channel);


#ifdef __cplusplus
}
#endif
