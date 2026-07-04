/* Includes */

#include "lib_types.h"

#include "HW_OPAMP.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

typedef struct
{
    // Library-owned mutable HAL handle (HAL_OPAMP_* and self-calibration
    // mutate state inside it — trims, state machine). Copied from the
    // user's const config at init.
    OPAMP_HandleTypeDef hopamp;
} HW_OPAMP_channelData_S;

typedef struct
{
    const HW_OPAMP_config_S * config;

    HW_OPAMP_channelData_S channelData[HW_OPAMP_CHANNEL_COUNT];
    bool initialized;
} HW_OPAMP_data_S;

/* Private Function Declarations */

static bool HW_OPAMP_private_initOneChannel(HW_OPAMP_channels_E ch);

/* Private Data Definitions */

static HW_OPAMP_data_S HW_OPAMP_data;
static HW_OPAMP_data_S * const data = &HW_OPAMP_data;

/* Private Function Definitions */

// [impl->fw~hal_opamp_002~1]
static bool HW_OPAMP_private_initOneChannel(HW_OPAMP_channels_E ch)
{
    bool ret = true;
    OPAMP_HandleTypeDef * const hopamp = &data->channelData[ch].hopamp;

    // Copy the user handle into mutable storage; the calibration sequence
    // and HAL state machine write into it.
    *hopamp = data->config->channels[ch].hopamp;

    // Self-calibration requires the internal ADC output disabled (the HAL
    // returns HAL_ERROR otherwise), so init once with it forced off.
    hopamp->Init.InternalOutput = DISABLE;
    if (HAL_OPAMP_Init(hopamp) != HAL_OK)
    {
        ret = false;
    }

    // Input-offset self-calibration. Blocks ~25 ms on the HAL tick; it
    // writes TrimmingValueN/P into the handle and flips UserTrimming to
    // OPAMP_TRIMMING_USER so the following re-init applies the new trims.
    if (ret)
    {
        if (HAL_OPAMP_SelfCalibrate(hopamp) != HAL_OK)
        {
            ret = false;
        }
    }

    // Re-init with the internal output enabled (legal from the READY
    // state calibration leaves us in); re-applies every field including
    // the freshly measured trims, routing the amplifier output to its
    // internal ADC input.
    if (ret)
    {
        hopamp->Init.InternalOutput = ENABLE;
        if (HAL_OPAMP_Init(hopamp) != HAL_OK)
        {
            ret = false;
        }
    }

    if (ret)
    {
        if (HAL_OPAMP_Start(hopamp) != HAL_OK)
        {
            ret = false;
        }
    }

    return ret;
}

/* Public Function Definitions */

// [impl->fw~hal_opamp_001~1]
bool HW_OPAMP_init(const HW_OPAMP_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= HW_OPAMP_CHANNEL_COUNT))
    {
        data->config = config;

        bool success = true;
        for (uint8_t ch = 0U; ch < config->numChannels; ch++)
        {
            if (!HW_OPAMP_private_initOneChannel((HW_OPAMP_channels_E)ch))
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->initialized = true;
            ret = true;
        }
    }
    return ret;
}
