/* Includes */
#include "app_server.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lib_build.h"
#include "lib_utils.h"

#include "HW_ADC.h"
#include "app_motorControl.h"

/* Defines */

// Trace-service resources (fw~conn_trace_001). Link budget is 90% of the
// USB FS bulk ceiling. The RAM point (32 watches + 2 KB ring) matches the
// 256-byte Samples cap exactly (32 x 8 B worst tick) and leaves SRAM
// headroom for the motor-control application.
#define APP_SERVER_WATCH_CAPACITY          (32U)
#define APP_SERVER_SAMPLE_RAM_BYTES        (2048U)
#define APP_SERVER_LINK_BUDGET_BYTES_PER_S (1100000U)

// VBUS sense front end (board dividers). Voltage: 0.15 V/V divider.
// Current: INA180A2 over 12 mOhm, 0.6 V/A.
#define APP_SERVER_VBUS_V_CHANNEL  (HW_ADC_CHANNEL_1)
#define APP_SERVER_VBUS_V_INPUT    (12U)
#define APP_SERVER_VBUS_V_RATIO    (0.15f)
#define APP_SERVER_VBUS_I_CHANNEL  (HW_ADC_CHANNEL_2)
#define APP_SERVER_VBUS_I_INPUT    (11U)
#define APP_SERVER_VBUS_I_V_PER_A  (0.6f)

/* Private Function Declarations */

static void app_server_config_private_handleRequest(const board_Request * const request,
                                                    shared_Response * const response);
static bool app_server_config_private_buildTelemetry(board_Telemetry * const telemetry);

/* Private Data Definitions */

static app_server_watch_S app_server_config_watchStorage[2U * APP_SERVER_WATCH_CAPACITY];
static uint8_t app_server_config_sampleStorage[APP_SERVER_SAMPLE_RAM_BYTES];

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
// 1 kHz counter task_1ms increments — the SIL trace scenarios' known signal;
// the rest is scratch for read/write scenarios.
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

// Single-instance config: the server answers the CDC protocol channel; the
// board hooks below carry this board's telemetry assembly and command
// handling.
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
    .buildTelemetry = app_server_config_private_buildTelemetry,
};

/* Private Function Definitions */

// Board commands write the same requested-mode/setpoint state the on-device
// controls write; the most recent command prevails (sys~ops_002).
static void app_server_config_private_handleRequest(const board_Request * const request,
                                                    shared_Response * const response)
{
    response->accepted = false;
    switch (request->which_command)
    {
        // [impl->fw~conn_server_002~1]
        case board_Request_set_mode_tag:
        {
            app_motorControl_mode_E requestedMode = APP_MOTORCONTROL_MODE_OFF;
            bool modeKnown = true;
            switch (request->command.set_mode.mode)
            {
                case board_Mode_MODE_OFF:
                    requestedMode = APP_MOTORCONTROL_MODE_OFF;
                    break;
                case board_Mode_MODE_SIX_STEP_TRAP:
                    requestedMode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP;
                    break;
                default:
                    modeKnown = false;
                    break;
            }

            if (!modeKnown)
            {
                (void) strcpy(response->cause, "unknown mode");
            }
            else if (requestedMode == APP_MOTORCONTROL_MODE_OFF)
            {
                // MODE_OFF is the disable request (fw~mc_006), never a method
                // selection: a stop is always accepted.
                app_motorControl_setMode(APP_MOTORCONTROL_CHANNEL_MAIN, APP_MOTORCONTROL_MODE_OFF);
                response->accepted = true;
            }
            else
            {
                // Method values: selection only while the bridge is disabled
                // (sys~mc_005) and never while faulted (button-path parity,
                // fw~mc_007) — an accepted-but-inert command would lie.
                app_motorControl_snapshot_S snapshot = { 0 };
                taskENTER_CRITICAL();
                (void) app_motorControl_getSnapshot(APP_MOTORCONTROL_CHANNEL_MAIN, &snapshot);
                taskEXIT_CRITICAL();

                if (snapshot.state == APP_MOTORCONTROL_STATE_FAULTED)
                {
                    (void) strcpy(response->cause, "fault latched");
                }
                else if ((snapshot.state == APP_MOTORCONTROL_STATE_ENABLED) &&
                         (requestedMode != snapshot.mode))
                {
                    (void) strcpy(response->cause, "bridge enabled");
                }
                else
                {
                    app_motorControl_setMode(APP_MOTORCONTROL_CHANNEL_MAIN, requestedMode);
                    response->accepted = true;
                }
            }
            break;
        }

        // [impl->fw~conn_server_003~1]
        case board_Request_set_velocity_tag:
        {
            const float32_t velocity_radPerSec = request->command.set_velocity.velocity_radps;
            if ((isfinite(velocity_radPerSec)) &&
                (fabsf(velocity_radPerSec) <= APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC))
            {
                app_motorControl_setVelocity(APP_MOTORCONTROL_CHANNEL_MAIN, velocity_radPerSec);
                response->accepted = true;
            }
            else
            {
                (void) strcpy(response->cause, "velocity out of range");
            }
            break;
        }

        // [impl->fw~conn_server_004~1]
        case board_Request_clear_fault_tag:
            // Idempotent: clearing an already-clear latch succeeds.
            app_motorControl_clearFault(APP_MOTORCONTROL_CHANNEL_MAIN);
            response->accepted = true;
            break;

        default:
            (void) strcpy(response->cause, "unknown command");
            break;
    }
}

// [impl->fw~obs_status_001~1]
static bool app_server_config_private_buildTelemetry(board_Telemetry * const telemetry)
{
    app_motorControl_snapshot_S snapshot = { 0 };
    // Snapshot under a critical section: the 1 ms control task outranks the
    // server task and could otherwise tear the multi-word copy.
    taskENTER_CRITICAL();
    (void) app_motorControl_getSnapshot(APP_MOTORCONTROL_CHANNEL_MAIN, &snapshot);
    taskEXIT_CRITICAL();

    float32_t vbusV_v = 0.0f;
    float32_t vbusI_v = 0.0f;
    // TODO - add some sort of IO_voltageMonitor module to add _logical_ ADC channels and voltage scaling and conversion to non-Volt units (ie, current)
    (void) HW_ADC_getVolts(APP_SERVER_VBUS_V_CHANNEL, APP_SERVER_VBUS_V_INPUT, &vbusV_v);
    (void) HW_ADC_getVolts(APP_SERVER_VBUS_I_CHANNEL, APP_SERVER_VBUS_I_INPUT, &vbusI_v);

    telemetry->timestamp_ms = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
    telemetry->mode = ((snapshot.mode == APP_MOTORCONTROL_MODE_SIX_STEP_TRAP))
                          ? board_Mode_MODE_SIX_STEP_TRAP
                          : board_Mode_MODE_OFF;
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
    return true;
}
