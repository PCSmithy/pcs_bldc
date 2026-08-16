/* Includes */
#include "app_server.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "HW_ADC.h"
#include "app_motorControl.h"

/* Defines */

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

/* Public Data Definitions */

// Single-instance config: the server answers the CDC protocol channel; the
// board hooks below carry this board's telemetry assembly and command
// handling.
const app_server_config_S app_server_config =
{
    .frame          = IO_COBSFRAME_CHANNEL_CDC,
    .serial         = IO_SERIAL_CHANNEL_CDC,
    .handleRequest  = app_server_config_private_handleRequest,
    .buildTelemetry = app_server_config_private_buildTelemetry,
};

/* Private Function Definitions */

// Board commands land with the control-service work; until then every
// command is rejected.
static void app_server_config_private_handleRequest(const board_Request * const request,
                                                    shared_Response * const response)
{
    (void) request;
    response->accepted = false;
    (void) strcpy(response->cause, "not implemented");
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
