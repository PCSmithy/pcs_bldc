/* Includes */

#include "app_userControls.h"

/* Public Data Definitions */

// Single-instance config: wires the user controls to the board's button, dial
// encoder, the motor-control channel they command, and the LED ring whose
// display mode a double-tap cycles.
const app_userControls_config_S app_userControls_config =
{
    .motor  = APP_MOTORCONTROL_CHANNEL_MAIN,
    .button = DEV_SWITCH_CHANNEL_USER_BUTTON,
    .dial   = IO_AS5048_CHANNEL_DIAL,
    .ring   = APP_RGBLEDRING_CHANNEL_RING,
};
