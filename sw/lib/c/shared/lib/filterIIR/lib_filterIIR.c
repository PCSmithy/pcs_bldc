
/* Includes */
#include "lib_filterIIR.h"


/* Defines */

/* Typedefs */


/* Private Function Declarations */

/* Private Data Definitions */

/* Private Function Definitions */

/* Public Function Definitions */

bool lib_filterIIR_init(lib_filterIIR_channel_S * const filter)
{
    bool success = false;
    if (filter != NULL)
    {
        filter->init = false;
        switch (filter->type)
        {
            case LIB_FILTERIIR_TYPE_EMA:
            {
                lib_filterIIR_ema_S * const ema = &filter->ema;
                if ((ema->alpha > 0.0f) && (ema->alpha <= 1.0f))
                {
                    ema->y_k = ema->x_k;
                    ema->y_k_1 = ema->x_k;
                    filter->init = true;
                    success = true;
                }
                break;
            }
            // case LIB_FILTERIIR_TYPE_BUTTERWORTH: // TODO
            // case LIB_FILTERIIR_TYPE_CHEBYSHEV:
            // case LIB_FILTERIIR_TYPE_CHEBYSHEV_II:
            // case LIB_FILTERIIR_TYPE_BESSEL:
            case LIB_FILTERIIR_TYPE_COUNT:
            default:
                break;
        }
    }
    return success;
}

void lib_filterIIR_update(lib_filterIIR_channel_S * const filter)
{
    if ((filter != NULL) && (filter->init))
    {
        switch (filter->type)
        {
            case LIB_FILTERIIR_TYPE_EMA:
            {
                lib_filterIIR_ema_S * const ema = &filter->ema;
                const float32_t a = ema->alpha;

                ema->y_k_1 = ema->y_k;
                ema->y_k = ((1.0f - a) * ema->y_k_1) + (a * ema->x_k);
                break;
            }
            // case LIB_FILTERIIR_TYPE_BUTTERWORTH: // TODO
            // case LIB_FILTERIIR_TYPE_CHEBYSHEV:
            // case LIB_FILTERIIR_TYPE_CHEBYSHEV_II:
            // case LIB_FILTERIIR_TYPE_BESSEL:
            case LIB_FILTERIIR_TYPE_COUNT:
            default:
                break;
        }
    }
}
