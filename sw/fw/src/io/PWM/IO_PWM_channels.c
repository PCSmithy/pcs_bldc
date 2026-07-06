/* Includes */
#include "IO_PWM.h"
#include "HW_TIM.h"
#include "lib_utils.h"

/* Defines */


/* Private Function Declarations */

/* Private Data Declarations */

// All three phases live on TIM1 (HW_TIM_CHANNEL_1), one complementary
// output-compare unit each. The unit index matches the TIM1 OC array order in
// HW_TIM_channels: unit 0 = CH1 = phase U (PE9), unit 1 = CH2 = phase V (PE11),
// unit 2 = CH3 = phase W (PE13).
static const IO_PWM_phaseConfig_S IO_PWM_phaseConfig[] =
{
    [IO_PWM_PHASE_U] = { .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 0U },
    [IO_PWM_PHASE_V] = { .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 1U },
    [IO_PWM_PHASE_W] = { .timChannel = HW_TIM_CHANNEL_1, .ocUnit = 2U },
};

const IO_PWM_config_S IO_PWM_config =
{
    .phases           = IO_PWM_phaseConfig,
    .numPhases        = COUNTOF(IO_PWM_phaseConfig),
    .bridgeTimChannel = HW_TIM_CHANNEL_1,
};

/* Private Function Definitions */


/* Public Function Definitions */
