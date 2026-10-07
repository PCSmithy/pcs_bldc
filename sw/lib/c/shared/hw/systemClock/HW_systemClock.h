#pragma once

/* Includes */
#include "lib_types.h"

/* Target Config */
#include "HW_systemClock_target.h"   // HW_systemClock_config_S

#ifdef __cplusplus
extern "C" {
#endif

/* Public Function Declarations */
bool HW_systemClock_init(const HW_systemClock_config_S * const config);
#ifdef __cplusplus
}
#endif
