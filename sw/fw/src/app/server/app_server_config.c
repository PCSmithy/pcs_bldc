/* Includes */
#include "app_server.h"

#include <math.h>
#include <stddef.h>
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

#define APP_SERVER_MODE_BIT(mode) (1UL << (uint32_t) (mode))
#define APP_SERVER_KIND_BIT(kind) (1UL << (uint32_t) (kind))
#define APP_SERVER_DRIVING_MODES  (APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP) | \
                                   APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_SINUSOIDAL) | \
                                   APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_FOC_TORQUE) | \
                                   APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_FOC_VELOCITY) | \
                                   APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_FOC_POSITION))
#define APP_SERVER_ALL_MODES      (APP_SERVER_DRIVING_MODES | APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_OFF))

/* Typedefs */

// A Command's setpoints stream into the sequencer as they decode
// (board.options); the first fault found rides here to the verdict.
typedef struct
{
    uint32_t kinds;   // APP_SERVER_KIND_BIT of each kind staged
    bool     rejected;
    char     cause[sizeof(((shared_Response *) 0)->cause)];
} app_server_config_staging_S;

// One row per Setpoint target arm (fw~conn_server_007): the modes that take it,
// and the rating bounding its value (none for the stop).
typedef struct
{
    pb_size_t            tag;
    app_sequencer_kind_E kind;
    uint32_t             modes;        // APP_SERVER_MODE_BIT per accepting mode
    bool                 bounded;      // |value| <= the rating at valueBound
    size_t               valueBound;   // offsetof in app_motorControl_ratings_S
    bool                 isMove;       // its limit a velocity limit, bounding the velocity at the point (fw~mc_030)
} app_server_config_setpointKind_S;

/* Static Assertions */

// The wire mode and the motor-control mode share their values (mapMode casts).
_Static_assert((int) _board_Mode_ARRAYSIZE == (int) APP_MOTORCONTROL_MODE_COUNT, "board.Mode and the motor modes differ in count");
_Static_assert((board_Mode_MODE_OFF == (board_Mode) APP_MOTORCONTROL_MODE_OFF) &&
               (board_Mode_MODE_SIX_STEP_TRAP == (board_Mode) APP_MOTORCONTROL_MODE_SIX_STEP_TRAP) &&
               (board_Mode_MODE_SINUSOIDAL == (board_Mode) APP_MOTORCONTROL_MODE_SINUSOIDAL) &&
               (board_Mode_MODE_FOC_TORQUE == (board_Mode) APP_MOTORCONTROL_MODE_FOC_TORQUE) &&
               (board_Mode_MODE_FOC_VELOCITY == (board_Mode) APP_MOTORCONTROL_MODE_FOC_VELOCITY) &&
               (board_Mode_MODE_FOC_POSITION == (board_Mode) APP_MOTORCONTROL_MODE_FOC_POSITION),
               "board.Mode and app_motorControl_mode_E differ in value");

/* Private Function Declarations */

static bool app_server_config_private_handleRequest(const board_Request * const request,
                                                    board_Reply * const reply,
                                                    shared_Response * const response);
static bool app_server_config_private_prepareCommand(pb_istream_t * stream, const pb_field_t * field, void ** arg);
static bool app_server_config_private_buildTelemetry(board_Telemetry * const telemetry);

/* Private Data Definitions */

static app_server_watch_S app_server_config_watchStorage[2U * APP_SERVER_WATCH_CAPACITY];
static app_server_config_staging_S app_server_config_staging;

static const app_server_config_setpointKind_S app_server_config_setpointKinds[] =
{
    { .tag = board_Setpoint_velocity_radps_tag, .kind = APP_SEQUENCER_KIND_VELOCITY,
      .modes = APP_SERVER_DRIVING_MODES,
      .bounded = true, .valueBound = offsetof(app_motorControl_ratings_S, maxVelocity_radPerSec), .isMove = false },
    { .tag = board_Setpoint_torque_nm_tag, .kind = APP_SEQUENCER_KIND_TORQUE,
      .modes = APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_FOC_TORQUE),
      .bounded = true, .valueBound = offsetof(app_motorControl_ratings_S, maxTorque_Nm), .isMove = false },
    { .tag = board_Setpoint_stop_tag, .kind = APP_SEQUENCER_KIND_STOP,
      .modes = APP_SERVER_ALL_MODES,
      .bounded = false, .valueBound = 0U, .isMove = false },
    { .tag = board_Setpoint_position_rad_tag, .kind = APP_SEQUENCER_KIND_POSITION,
      .modes = APP_SERVER_MODE_BIT(APP_MOTORCONTROL_MODE_FOC_POSITION),
      .bounded = true, .valueBound = offsetof(app_motorControl_ratings_S, maxPosition_rad), .isMove = true },
};

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

// The rating a setpoint-kind row bounds its value by.
static float32_t app_server_config_private_rating(const app_motorControl_ratings_S * const ratings, size_t offset)
{
    float32_t rating = 0.0f;
    (void) memcpy(&rating, &((const uint8_t *) ratings)[offset], sizeof(rating));
    return rating;
}

static const char * app_server_config_private_appendCause(app_sequencer_append_E result)
{
    const char * cause = NULL;
    switch (result)
    {
        case APP_SEQUENCER_APPEND_OK:           break;
        case APP_SEQUENCER_APPEND_FULL:         cause = "sequence exceeds the player's capacity"; break;
        case APP_SEQUENCER_APPEND_OUT_OF_ORDER: cause = "setpoint times not in order"; break;
        case APP_SEQUENCER_APPEND_NOT_STAGING:  cause = "sequence player busy"; break;
        default:                                cause = "setpoint has no target"; break;
    }
    return cause;
}

// One Command.setpoints element, checked against fw~conn_server_007's table and
// appended to the staged sequence; the first fault stands for the verdict.
static void app_server_config_private_stageSetpoint(const board_Setpoint * const setpoint)
{
    app_server_config_staging_S * const staging = &app_server_config_staging;
    const app_server_config_setpointKind_S * row = NULL;
    for (size_t i = 0U; i < COUNTOF(app_server_config_setpointKinds); i++)
    {
        if (app_server_config_setpointKinds[i].tag == setpoint->which_target)
        {
            row = &app_server_config_setpointKinds[i];
        }
    }
    app_motorControl_ratings_S ratings = { 0 };
    (void) app_motorControl_getRatings(APP_MOTORCONTROL_CHANNEL_MAIN, &ratings);

    // The float target arms share the union's first word; the stop carries none.
    const bool hasValue = (row != NULL) && row->bounded;
    const bool isMove = (row != NULL) && row->isMove;
    const float32_t value = hasValue ? setpoint->target.velocity_radps : 0.0f;
    const float32_t limit = setpoint->rate_limit;
    const float32_t accel = isMove ? setpoint->accel_limit_radps2 : 0.0f;
    const float32_t atPoint = isMove ? setpoint->velocity_at_point_radps : 0.0f;
    const float32_t atPointBound = (limit > 0.0f) ? limit : ratings.maxVelocity_radPerSec;

    const char * cause = NULL;
    if (row == NULL)
    {
        cause = "setpoint has no target";
    }
    else if ((!isfinite(value)) || (!isfinite(limit)) || (!isfinite(accel)) || (!isfinite(atPoint)))
    {
        cause = "setpoint not finite";
    }
    else if ((limit < 0.0f) || (accel < 0.0f))
    {
        cause = "rate limit negative";
    }
    else if (hasValue && (fabsf(value) > app_server_config_private_rating(&ratings, row->valueBound)))
    {
        cause = "setpoint exceeds the motor rating";
    }
    else if (isMove && (limit > ratings.maxVelocity_radPerSec))
    {
        cause = "velocity limit exceeds the motor rating";
    }
    else if (isMove && (fabsf(atPoint) > atPointBound))
    {
        cause = "velocity at the point exceeds the velocity limit";
    }
    else
    {
        app_sequencer_setpoint_S entry = { .t_us = setpoint->t_us, .kind = row->kind };
        if (isMove)
        {
            entry.move = (app_sequencer_move_S){ value, limit, accel, atPoint };
        }
        else
        {
            entry.target = (app_sequencer_target_S){ value, limit };
        }
        cause = app_server_config_private_appendCause(app_sequencer_append(APP_SEQUENCER_CHANNEL_MAIN, &entry));
        staging->kinds |= APP_SERVER_KIND_BIT(row->kind);
    }

    if ((cause != NULL) && (!staging->rejected))
    {
        staging->rejected = true;
        (void) strcpy(staging->cause, cause);
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
        if (!app_sequencer_begin(APP_SEQUENCER_CHANNEL_MAIN))
        {
            staging->rejected = true;
            (void) strcpy(staging->cause, "sequence player busy");   // a commit awaits its pass
        }
        command->setpoints.funcs.decode = app_server_config_private_setpointCallback;
        command->setpoints.arg = NULL;
    }
    return true;
}

// The board.Mode value as the motor control's mode, which shares its values;
// false for a value that names no member.
static bool app_server_config_private_mapMode(board_Mode wire, app_motorControl_mode_E * const mode)
{
    const bool known = ((uint32_t) wire < (uint32_t) APP_MOTORCONTROL_MODE_COUNT);
    if (known)
    {
        *mode = (app_motorControl_mode_E) wire;
    }
    return known;
}

// The APP_SERVER_KIND_BIT set of the setpoint kinds a mode takes.
static uint32_t app_server_config_private_kindsTaken(app_motorControl_mode_E mode)
{
    uint32_t kinds = 0U;
    for (size_t i = 0U; i < COUNTOF(app_server_config_setpointKinds); i++)
    {
        if ((app_server_config_setpointKinds[i].modes & APP_SERVER_MODE_BIT(mode)) != 0U)
        {
            kinds |= APP_SERVER_KIND_BIT(app_server_config_setpointKinds[i].kind);
        }
    }
    return kinds;
}

// Why a mode request is blocked, or NULL: a method never while a fault is
// latched (fw~mc_007), and while enabled only the running one (sys~mc_005).
// Called inside the critical section that acts, so the check cannot go stale.
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
            else if ((app_server_config_staging.kinds & ~app_server_config_private_kindsTaken(mode)) != 0U)
            {
                cause = "setpoint kind not for this mode";
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
    telemetry->mode = (snapshot.mode < APP_MOTORCONTROL_MODE_COUNT) ? (board_Mode) snapshot.mode : board_Mode_MODE_OFF;
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
