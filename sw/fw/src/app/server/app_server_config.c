/* Includes */
#include "app_server.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lib_build.h"
#include "lib_utils.h"

#include "pb_decode.h"

#include "HW_ADC.h"
#include "app_motorControl.h"
#include "app_sequencer.h"

/* Defines */

// Trace resources (fw~conn_trace_001): the link budget is the bench-measured
// sustained rate for 256-byte messages (515 kB/s, tools/pcs_client.py
// --link-test, 2026-09-15) less margin; the server's per-frame CPU cost, not
// USB, sets it. Reference list both budgets admit: 4 x 4 B every cycle beside
// 28 x 8 B every 200 cycles (u = 628 B/ms of 2048, r = 378850 B/s of 480000).
#define APP_SERVER_WATCH_CAPACITY          (32U)
#define APP_SERVER_SAMPLE_RAM_BYTES        (2048U)
#define APP_SERVER_LINK_BUDGET_BYTES_PER_S (480000U)

// VBUS sense front end (board dividers). Voltage: 0.15 V/V divider.
// Current: INA180A2 over 12 mOhm, 0.6 V/A.
#define APP_SERVER_VBUS_V_CHANNEL  (HW_ADC_CHANNEL_1)
#define APP_SERVER_VBUS_V_INPUT    (12U)
#define APP_SERVER_VBUS_V_RATIO    (0.15f)
#define APP_SERVER_VBUS_I_CHANNEL  (HW_ADC_CHANNEL_2)
#define APP_SERVER_VBUS_I_INPUT    (11U)
#define APP_SERVER_VBUS_I_V_PER_A  (0.6f)

/* Typedefs */

// A Command's setpoints stream into the sequencer as they decode
// (board.options); the first fault found rides here to the verdict.
typedef struct
{
    bool     open;          // the sequencer took begin for this Command
    uint32_t count;
    uint32_t lastTime_us;
    bool     rejected;
    char     cause[sizeof(((shared_Response *) 0)->cause)];
} app_server_config_staging_S;

/* Private Function Declarations */

static bool app_server_config_private_handleRequest(const board_Request * const request,
                                                    board_Reply * const reply,
                                                    shared_Response * const response);
static bool app_server_config_private_prepareCommand(pb_istream_t * stream, const pb_field_t * field, void ** arg);
static bool app_server_config_private_buildTelemetry(board_Telemetry * const telemetry);

/* Private Data Definitions */

static app_server_watch_S app_server_config_watchStorage[2U * APP_SERVER_WATCH_CAPACITY];
static app_server_config_staging_S app_server_config_staging;
static uint8_t app_server_config_sampleStorage[APP_SERVER_TRACE_STORAGE_BYTES(APP_SERVER_SAMPLE_RAM_BYTES)];

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)

// Identity-mapped MCU memory: all of SRAM readable and writable, all of
// flash read-only.
static const app_server_region_S app_server_config_readableRegions[] =
{
    { .start = 0x20000000U, .length = 32U * 1024U,  .base = 0x20000000U },
    { .start = 0x08000000U, .length = 128U * 1024U, .base = 0x08000000U },
};
static const app_server_region_S app_server_config_writableRegions[] =
{
    { .start = 0x20000000U, .length = 32U * 1024U, .base = 0x20000000U },
};

#elif (BUILD_TARGET == BUILD_TARGET_SIM)

// Hosted pointers don't fit the 32-bit protocol address space, so the sim
// backs an SRAM-like protocol range with a dedicated window. Word [0] is a
// 1 kHz counter task_1ms increments; words [1]/[2] are a per-PWM-cycle counter
// and its complement the bridge cycle callback writes - the SIL trace
// scenarios' known signals; the rest is scratch for read/write scenarios.
uint32_t app_server_simTraceWindow32[256];

static const app_server_region_S app_server_config_readableRegions[] =
{
    { .start = 0x20000000U, .length = sizeof(app_server_simTraceWindow32),
      .base = (uintptr_t) app_server_simTraceWindow32 },
};
static const app_server_region_S app_server_config_writableRegions[] =
{
    { .start = 0x20000000U, .length = sizeof(app_server_simTraceWindow32),
      .base = (uintptr_t) app_server_simTraceWindow32 },
};

#endif

/* Public Data Definitions */

// The server answers the CDC protocol channel with this board's hooks.
const app_server_config_S app_server_config =
{
    .frame          = IO_COBSFRAME_CHANNEL_CDC,
    .serial         = IO_SERIAL_CHANNEL_CDC,

    .readableRegions     = app_server_config_readableRegions,
    .readableRegionCount = COUNTOF(app_server_config_readableRegions),
    .writableRegions     = app_server_config_writableRegions,
    .writableRegionCount = COUNTOF(app_server_config_writableRegions),
    .watchStorage        = app_server_config_watchStorage,
    .watchCapacity       = APP_SERVER_WATCH_CAPACITY,
    .sampleStorage       = app_server_config_sampleStorage,
    .sampleRamBudgetBytes = APP_SERVER_SAMPLE_RAM_BYTES,
    .linkBudgetBytesPerS  = APP_SERVER_LINK_BUDGET_BYTES_PER_S,

    .handleRequest  = app_server_config_private_handleRequest,
    .prepareCommand = app_server_config_private_prepareCommand,
    .buildTelemetry = app_server_config_private_buildTelemetry,
    // [impl->fw~conn_trace_008~1] the cycle sampler runs above the FreeRTOS
    // syscall priority, so this hook masks interrupts globally to hold it off.
    .setSamplerMasked = HW_ADC_setInjectedIrqMasked,
};

/* Private Function Definitions */

static void app_server_config_private_stageReject(const char * const cause)
{
    app_server_config_staging_S * const staging = &app_server_config_staging;
    if (!staging->rejected)
    {
        staging->rejected = true;
        (void) strcpy(staging->cause, cause);
    }
}

// One Command.setpoints element, checked against the rejection table of
// fw~conn_server_007 and appended to the staged sequence. The first fault
// stands while the rest still decode, so the verdict names it.
static void app_server_config_private_stageSetpoint(const board_Setpoint * const setpoint)
{
    app_server_config_staging_S * const staging = &app_server_config_staging;
    app_motorControl_ratings_S ratings = { 0 };
    (void) app_motorControl_getRatings(APP_MOTORCONTROL_CHANNEL_MAIN, &ratings);

    app_sequencer_setpoint_S entry = {
        .t_us = setpoint->t_us,
        .kind = APP_SEQUENCER_KIND_COUNT,   // until the target arm names one
        .value = 0.0f,
        .rateLimit = setpoint->rate_limit,
        .accelLimit = 0.0f,
        .velocityAtPoint = 0.0f,
    };
    float32_t bound = 0.0f;
    // a position's rate limit is its velocity limit, itself bounded, and bounds
    // its velocity at the point (the maximum velocity for a limit of zero)
    float32_t limitBound = INFINITY;
    float32_t velocityAtPointBound = INFINITY;
    switch (setpoint->which_target)
    {
        case board_Setpoint_velocity_radps_tag:
            entry.kind = APP_SEQUENCER_KIND_VELOCITY;
            entry.value = setpoint->target.velocity_radps;
            bound = ratings.maxVelocity_radPerSec;
            break;

        case board_Setpoint_torque_nm_tag:
            entry.kind = APP_SEQUENCER_KIND_TORQUE;
            entry.value = setpoint->target.torque_nm;
            bound = ratings.maxTorque_Nm;
            break;

        case board_Setpoint_stop_tag:
            entry.kind = APP_SEQUENCER_KIND_STOP;   // a zero value within a zero bound
            break;

        case board_Setpoint_position_rad_tag:
            entry.kind = APP_SEQUENCER_KIND_POSITION;
            entry.value = setpoint->target.position_rad;
            entry.accelLimit = setpoint->accel_limit_radps2;
            entry.velocityAtPoint = setpoint->velocity_at_point_radps;
            bound = ratings.maxPosition_rad;
            limitBound = ratings.maxVelocity_radPerSec;
            velocityAtPointBound = (entry.rateLimit > 0.0f) ? entry.rateLimit : ratings.maxVelocity_radPerSec;
            break;

        default:
            break;
    }

    if (entry.kind == APP_SEQUENCER_KIND_COUNT)
    {
        app_server_config_private_stageReject("setpoint has no target");
    }
    else if ((!isfinite(entry.value)) || (!isfinite(entry.rateLimit)) ||
             (!isfinite(entry.accelLimit)) || (!isfinite(entry.velocityAtPoint)))
    {
        app_server_config_private_stageReject("setpoint not finite");
    }
    else if ((entry.rateLimit < 0.0f) || (entry.accelLimit < 0.0f))
    {
        app_server_config_private_stageReject("rate limit negative");
    }
    else if (fabsf(entry.value) > bound)
    {
        app_server_config_private_stageReject("setpoint exceeds the motor rating");
    }
    else if (entry.rateLimit > limitBound)
    {
        app_server_config_private_stageReject("velocity limit exceeds the motor rating");
    }
    else if (fabsf(entry.velocityAtPoint) > velocityAtPointBound)
    {
        app_server_config_private_stageReject("velocity at the point exceeds the velocity limit");
    }
    else if ((staging->count > 0U) && (entry.t_us < staging->lastTime_us))
    {
        app_server_config_private_stageReject("setpoint times not in order");
    }
    else if (staging->open)
    {
        if (app_sequencer_append(APP_SEQUENCER_CHANNEL_MAIN, &entry))
        {
            staging->count++;
            staging->lastTime_us = entry.t_us;
        }
        else
        {
            app_server_config_private_stageReject("sequence exceeds the player's capacity");
        }
    }
    else
    {
        // Not open: the busy rejection already stands.
    }
}

// nanopb callback: one repeated Command.setpoints element per call.
static bool app_server_config_private_setpointCallback(pb_istream_t * stream, const pb_field_t * field, void ** arg)
{
    (void) field;
    (void) arg;
    board_Setpoint setpoint = board_Setpoint_init_zero;
    const bool ret = pb_decode(stream, board_Setpoint_fields, &setpoint);
    if (ret)
    {
        app_server_config_private_stageSetpoint(&setpoint);
    }
    return ret;
}

// [impl->fw~conn_server_007~1] the Request's command-arm hook: a Command's
// setpoint list streams into the sequencer's staged table as it decodes. A
// second run arm in one Request starts the staging over, no merge.
static bool app_server_config_private_prepareCommand(pb_istream_t * stream, const pb_field_t * field, void ** arg)
{
    (void) stream;
    (void) arg;
    if (field->tag == board_Request_run_tag)
    {
        board_Command * const command = (board_Command *) field->pData;
        app_server_config_staging_S * const staging = &app_server_config_staging;
        *staging = (app_server_config_staging_S){ 0 };
        staging->open = app_sequencer_begin(APP_SEQUENCER_CHANNEL_MAIN);
        if (!staging->open)
        {
            app_server_config_private_stageReject("sequence player busy");   // a commit awaits its pass: retry
        }
        command->setpoints.funcs.decode = app_server_config_private_setpointCallback;
        command->setpoints.arg = NULL;
    }
    return true;
}

// The board.Mode value as the motor control's mode; false for a value that
// names no member.
static bool app_server_config_private_mapMode(board_Mode wire, app_motorControl_mode_E * const mode)
{
    bool known = true;
    switch (wire)
    {
        case board_Mode_MODE_OFF:           *mode = APP_MOTORCONTROL_MODE_OFF;           break;
        case board_Mode_MODE_SIX_STEP_TRAP: *mode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP; break;
        case board_Mode_MODE_SINUSOIDAL:    *mode = APP_MOTORCONTROL_MODE_SINUSOIDAL;    break;
        case board_Mode_MODE_FOC_TORQUE:    *mode = APP_MOTORCONTROL_MODE_FOC_TORQUE;    break;
        case board_Mode_MODE_FOC_VELOCITY:  *mode = APP_MOTORCONTROL_MODE_FOC_VELOCITY;  break;
        case board_Mode_MODE_FOC_POSITION:  *mode = APP_MOTORCONTROL_MODE_FOC_POSITION;  break;
        default:                            known = false;                               break;
    }
    return known;
}

// Why a mode request is blocked, or NULL. MODE_OFF is the disable request
// (fw~mc_006) and always passes; a method selects only while the bridge is
// disabled or already running it (sys~mc_005), never while a fault is latched
// (button-path parity, fw~mc_007). Called inside the critical section that
// also acts, so the 1 ms task cannot enable the bridge or latch a fault
// between check and act (bounded: compares + the stores).
static const char * app_server_config_private_modeBlocked(app_motorControl_mode_E mode)
{
    const char * cause = NULL;
    if (mode != APP_MOTORCONTROL_MODE_OFF)
    {
        app_motorControl_snapshot_S snapshot = { 0 };
        (void) app_motorControl_getSnapshot(APP_MOTORCONTROL_CHANNEL_MAIN, &snapshot);
        if (snapshot.state == APP_MOTORCONTROL_STATE_FAULTED)
        {
            cause = "fault latched";
        }
        else if ((snapshot.state == APP_MOTORCONTROL_STATE_ENABLED) && (mode != snapshot.mode))
        {
            cause = "bridge enabled";
        }
    }
    return cause;
}

// Board commands write the same requested-mode/setpoint state the on-device
// controls write; the most recent command prevails (sys~ops_002).
static bool app_server_config_private_handleRequest(const board_Request * const request,
                                                    board_Reply * const reply,
                                                    shared_Response * const response)
{
    bool isResult = false;
    const char * cause = NULL;
    response->accepted = false;
    switch (request->which_command)
    {
        // [impl->fw~conn_server_004~1]
        case board_Request_clear_fault_tag:
            // Idempotent: clearing an already-clear latch succeeds.
            app_motorControl_clearFault(APP_MOTORCONTROL_CHANNEL_MAIN);
            response->accepted = true;
            break;

        // [impl->fw~conn_server_007~1] the setpoints already sit in the
        // sequencer's staged table (prepareCommand); accepted, the mode request
        // and the commit land together, so the sequence starts under its mode.
        case board_Request_run_tag:
        {
            app_motorControl_mode_E mode = APP_MOTORCONTROL_MODE_OFF;
            if (!app_server_config_private_mapMode(request->command.run.mode, &mode))
            {
                cause = "unknown mode";
            }
            else if (app_server_config_staging.rejected)
            {
                cause = app_server_config_staging.cause;
            }
            else
            {
                taskENTER_CRITICAL();
                cause = app_server_config_private_modeBlocked(mode);
                if (cause != NULL)
                {
                    // blocked: the verdict carries why
                }
                else if (!app_sequencer_commit(APP_SEQUENCER_CHANNEL_MAIN))
                {
                    cause = "sequence not staged";   // the arm did not decode through prepareCommand
                }
                else
                {
                    app_motorControl_setMode(APP_MOTORCONTROL_CHANNEL_MAIN, mode);
                    response->accepted = true;
                }
                taskEXIT_CRITICAL();
            }
            break;
        }

        // [impl->fw~conn_server_008~1]
        case board_Request_get_motor_info_tag:
        {
            app_motorControl_ratings_S ratings = { 0 };
            if (app_motorControl_getRatings(APP_MOTORCONTROL_CHANNEL_MAIN, &ratings))
            {
                reply->which_result = board_Reply_motor_info_tag;
                reply->result.motor_info.kt_nm_per_a        = ratings.torqueConstant_NmPerA;
                reply->result.motor_info.i_max_a            = ratings.maxCurrent_A;
                reply->result.motor_info.max_torque_nm      = ratings.maxTorque_Nm;
                reply->result.motor_info.max_velocity_radps = ratings.maxVelocity_radPerSec;
                reply->result.motor_info.max_position_rad   = ratings.maxPosition_rad;
                isResult = true;
            }
            else
            {
                cause = "motor control not initialized";
            }
            break;
        }

        default:
            cause = "unknown command";
            break;
    }
    if (cause != NULL)
    {
        (void) strcpy(response->cause, cause);
    }
    return isResult;
}

// [impl->fw~obs_status_001~1]
static bool app_server_config_private_buildTelemetry(board_Telemetry * const telemetry)
{
    app_motorControl_snapshot_S snapshot = { 0 };
    // Snapshot under a critical section: the 1 ms control task outranks the
    // server task and could otherwise tear the multi-word copy.
    app_sequencer_status_S sequence = { 0 };
    taskENTER_CRITICAL();
    (void) app_motorControl_getSnapshot(APP_MOTORCONTROL_CHANNEL_MAIN, &snapshot);
    (void) app_sequencer_getStatus(APP_SEQUENCER_CHANNEL_MAIN, &sequence);
    taskEXIT_CRITICAL();

    float32_t vbusV_v = 0.0f;
    float32_t vbusI_v = 0.0f;
    // TODO - add some sort of IO_voltageMonitor module to add _logical_ ADC channels and voltage scaling and conversion to non-Volt units (ie, current)
    (void) HW_ADC_getVolts(APP_SERVER_VBUS_V_CHANNEL, APP_SERVER_VBUS_V_INPUT, &vbusV_v);
    (void) HW_ADC_getVolts(APP_SERVER_VBUS_I_CHANNEL, APP_SERVER_VBUS_I_INPUT, &vbusI_v);

    telemetry->timestamp_ms = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
    switch (snapshot.mode)
    {
        case APP_MOTORCONTROL_MODE_SIX_STEP_TRAP:
            telemetry->mode = board_Mode_MODE_SIX_STEP_TRAP;
            break;
        case APP_MOTORCONTROL_MODE_SINUSOIDAL:
            telemetry->mode = board_Mode_MODE_SINUSOIDAL;
            break;
        case APP_MOTORCONTROL_MODE_FOC_TORQUE:
            telemetry->mode = board_Mode_MODE_FOC_TORQUE;
            break;
        case APP_MOTORCONTROL_MODE_FOC_VELOCITY:
            telemetry->mode = board_Mode_MODE_FOC_VELOCITY;
            break;
        case APP_MOTORCONTROL_MODE_FOC_POSITION:
            telemetry->mode = board_Mode_MODE_FOC_POSITION;
            break;
        default:
            telemetry->mode = board_Mode_MODE_OFF;
            break;
    }
    switch (snapshot.state)
    {
        case APP_MOTORCONTROL_STATE_ENABLED:
            telemetry->state = board_DriveState_DRIVE_STATE_ENABLED;
            break;
        case APP_MOTORCONTROL_STATE_FAULTED:
            telemetry->state = board_DriveState_DRIVE_STATE_FAULTED;
            break;
        case APP_MOTORCONTROL_STATE_DISABLED:
        default:
            telemetry->state = board_DriveState_DRIVE_STATE_DISABLED;
            break;
    }
    telemetry->bus_voltage_v = vbusV_v / APP_SERVER_VBUS_V_RATIO;
    telemetry->bus_current_a = vbusI_v / APP_SERVER_VBUS_I_V_PER_A;
    telemetry->velocity_measured_radps = snapshot.velocityMeasured_radPerSec;
    telemetry->velocity_setpoint_radps = snapshot.velocitySetpoint_radPerSec;
    telemetry->torque_target_nm = snapshot.torqueTarget_Nm;
    telemetry->position_target_rad = snapshot.positionTarget_rad;
    telemetry->rotor_position_rad = snapshot.rotorPosition_rad;
    telemetry->sequence_running = sequence.running;
    telemetry->sequence_elapsed_ms = sequence.elapsed_ms;
    return true;
}
