/* Includes */
#include "HW_CORDIC.h"
#include "lib_build.h"  // BUILD_TARGET_STM32G4 / BUILD_TARGET_SIM constants

/* Private Data Definitions */
const HW_CORDIC_config_S HW_CORDIC_config =
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    .precisionCycles = 6U,   // 24 iterations: the float result's full width
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
    ._void = 0U,
#else
    #error "Unsupported BUILD_TARGET"
#endif
};
