/* Includes */
#include "HW_CORDIC.h"
#include "stm32g4xx_hal.h"

/* Defines */

// The HAL's CORDIC API polls with timeouts through a handle, far too slow
// for a per-PWM-cycle call; the peripheral is driven directly.
#define HW_CORDIC_PRECISION_MIN   (1U)
#define HW_CORDIC_PRECISION_MAX   (15U)

#define HW_CORDIC_TURNS_PER_RAD   (0.15915494309189535f)   // 1 / (2 pi)
#define HW_CORDIC_TURN_TO_WORD    (4294967296.0f)          // 2^32
#define HW_CORDIC_Q31_TO_FLOAT    (4.656612873077393e-10f) // 2^-31

/* Typedefs */

typedef struct
{
    bool initialized;
} HW_CORDIC_data_S;

/* Private Data Definitions */

static HW_CORDIC_data_S HW_CORDIC_data;
static HW_CORDIC_data_S * const data = &HW_CORDIC_data;

/* Public Function Definitions */

// [impl->fw~hal_cordic_001~1]
bool HW_CORDIC_init(const HW_CORDIC_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->precisionCycles >= HW_CORDIC_PRECISION_MIN) &&
        (config->precisionCycles <= HW_CORDIC_PRECISION_MAX))
    {
        __HAL_RCC_CORDIC_CLK_ENABLE();
        // Cosine function with one argument (the modulus defaults to one) and
        // two results, cosine then sine, both as 32-bit q1.31.
        CORDIC->CSR = ((uint32_t) config->precisionCycles << CORDIC_CSR_PRECISION_Pos) |
                      CORDIC_CSR_NRES;
        data->initialized = true;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_cordic_002~1]
bool HW_CORDIC_sinCos(float32_t theta_rad, float32_t * const sin_out, float32_t * const cos_out)
{
    bool ret = false;
    if ((data->initialized) && (sin_out != NULL) && (cos_out != NULL))
    {
        // The angle as a fraction of a turn, wrapped into [0, 1) by truncating
        // whole turns away. Scaled to the 32-bit word, the unsigned value is
        // the two's-complement q1.31 half-turn fraction the CORDIC reads as
        // [-pi, pi): the wrap costs no modulo.
        float32_t turns = theta_rad * HW_CORDIC_TURNS_PER_RAD;
        turns -= (float32_t) (int32_t) turns;
        if (turns < 0.0f)
        {
            turns += 1.0f;
        }
        if (turns >= 1.0f)
        {
            turns = 0.0f;   // a tiny negative angle rounded up to a whole turn
        }
        CORDIC->WDATA = (uint32_t) (turns * HW_CORDIC_TURN_TO_WORD);

        while ((CORDIC->CSR & CORDIC_CSR_RRDY) == 0U)
        {
            // precisionCycles clocks
        }
        *cos_out = (float32_t) (int32_t) CORDIC->RDATA * HW_CORDIC_Q31_TO_FLOAT;
        *sin_out = (float32_t) (int32_t) CORDIC->RDATA * HW_CORDIC_Q31_TO_FLOAT;
        ret = true;
    }
    return ret;
}
