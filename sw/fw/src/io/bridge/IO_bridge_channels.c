/* Includes */
#include "IO_bridge.h"
#include "HW_TIM.h"
#include "HW_ADC.h"
#include "lib_utils.h"

/* Defines */

// Phase-current sense front end: INA240A3 (100 V/V) across a 1 mOhm shunt ->
// 0.1 V/A, biased to a VREF/2 = 1.65 V zero-current midpoint (bipolar).
#define BRIDGE_PHASE_I_BIAS_V   (1.65f)
#define BRIDGE_PHASE_I_V_PER_A  (0.1f)
// DC-bus current sense front end: INA180A2 (50 V/V) across a 12 mOhm shunt ->
// 0.6 V/A, ground referenced (no bias).
#define BRIDGE_BUS_I_BIAS_V     (0.0f)
#define BRIDGE_BUS_I_V_PER_A    (0.6f)

/* Private Data Definitions */

// The single motor bridge maps its three phases onto TIM1's three complementary
// PWM channels: U -> CH1, V -> CH2, W -> CH3 (HW_TIM_CHANNEL_PWM_*). All three
// share TIM1, whose master output enable gates the whole bridge. Each phase
// shunt and the DC-bus shunt land on the board's ADC inputs: phase U on ADC1
// IN6, V on ADC2 IN7, W on ADC1 IN8, bus current on ADC2 IN11.
static const IO_bridge_channelConfig_S IO_bridge_channelConfig[] =
{
    [IO_BRIDGE_CHANNEL_MOTOR] =
    {
        .phaseU = HW_TIM_CHANNEL_PWM_U,
        .phaseV = HW_TIM_CHANNEL_PWM_V,
        .phaseW = HW_TIM_CHANNEL_PWM_W,

        .phaseCurrent =
        {
            [IO_BRIDGE_PHASE_U] = { HW_ADC_CHANNEL_1, 6U, BRIDGE_PHASE_I_BIAS_V, BRIDGE_PHASE_I_V_PER_A },
            [IO_BRIDGE_PHASE_V] = { HW_ADC_CHANNEL_2, 7U, BRIDGE_PHASE_I_BIAS_V, BRIDGE_PHASE_I_V_PER_A },
            [IO_BRIDGE_PHASE_W] = { HW_ADC_CHANNEL_1, 8U, BRIDGE_PHASE_I_BIAS_V, BRIDGE_PHASE_I_V_PER_A },
        },
        .busCurrent = { HW_ADC_CHANNEL_2, 11U, BRIDGE_BUS_I_BIAS_V, BRIDGE_BUS_I_V_PER_A },
    },
};

const IO_bridge_config_S IO_bridge_config =
{
    .channels    = IO_bridge_channelConfig,
    .numChannels = COUNTOF(IO_bridge_channelConfig),
};
