/* Includes */
#include "IO_PWM.h"
#include "HW_TIM.h"

#include "IO_PWM_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    const IO_PWM_config_S * config;
} IO_PWM_data_S;

/* Private Data Definitions */

static IO_PWM_data_S IO_PWM_data;
static IO_PWM_data_S * const data = &IO_PWM_data;

/* Private Function Declarations */

static uint32_t IO_PWM_private_dutyToCompare(float32_t duty, uint32_t period);

/* Private Function Definitions */

// Round duty x period to the nearest count. duty is pre-validated to [0, 1], so
// the result is bounded by period and needs no clamp. duty 1 maps to exactly
// the period (HW_TIM accepts compare == period); center-aligned PWM1 therefore
// falls one tick short of a true 100% at the counter peak, which the drive path
// tolerates.
static uint32_t IO_PWM_private_dutyToCompare(float32_t duty, uint32_t period)
{
    return (uint32_t)((duty * (float32_t)period) + 0.5f);
}

/* Public Function Definitions */

// [impl->fw~io_pwm_001~1]
bool IO_PWM_init(const IO_PWM_config_S * const config)
{
    bool success = false;

    if ((config != NULL) &&
        (config->phases != NULL) &&
        (config->numPhases <= IO_PWM_PHASE_COUNT) &&
        (config->bridgeTimChannel < HW_TIM_CHANNEL_COUNT))
    {
        success = true;
        for (size_t phase = 0U; (phase < config->numPhases) && success; phase++)
        {
            const IO_PWM_phaseConfig_S * const phaseConfig = &config->phases[phase];
            if ((phaseConfig->timChannel >= HW_TIM_CHANNEL_COUNT) ||
                (phaseConfig->ocUnit >= HW_TIM_OC_UNITS_PER_CHANNEL))
            {
                success = false;
            }
            else
            {
                // Enable the phase's output-compare unit up front; outputs stay
                // dark because HW_TIM commands MOE off at init, leaving the
                // bridge master output enable as the sole runtime gate.
                success = HW_TIM_setOutputEnabled(phaseConfig->timChannel,
                                                  phaseConfig->ocUnit, true);
            }
        }

        if (success)
        {
            data->config = config;
        }
    }

    return success;
}

// [impl->fw~io_pwm_002~1]
bool IO_PWM_setDuty(IO_PWM_phase_E phase, float32_t duty)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (phase < IO_PWM_PHASE_COUNT) &&
        (duty >= 0.0f) &&
        (duty <= 1.0f))
    {
        const IO_PWM_phaseConfig_S * const phaseConfig = &data->config->phases[phase];
        uint32_t period = 0U;
        if (HW_TIM_getPeriod(phaseConfig->timChannel, &period))
        {
            const uint32_t compare = IO_PWM_private_dutyToCompare(duty, period);
            ret = HW_TIM_setCompare(phaseConfig->timChannel, phaseConfig->ocUnit, compare);
        }
    }

    return ret;
}

// [impl->fw~io_pwm_003~1]
bool IO_PWM_setOutputEnabled(bool enabled)
{
    bool ret = false;
    if (data->config != NULL)
    {
        ret = HW_TIM_setMainOutputEnabled(data->config->bridgeTimChannel, enabled);
    }
    return ret;
}

// [impl->fw~io_pwm_003~1]
bool IO_PWM_getOutputEnabled(bool * const enabled)
{
    bool ret = false;
    if ((data->config != NULL) && (enabled != NULL))
    {
        ret = HW_TIM_getMainOutputEnabled(data->config->bridgeTimChannel, enabled);
    }
    return ret;
}
