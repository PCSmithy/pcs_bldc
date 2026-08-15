#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_systemClock_config.h"

/* Target Config */
#include "HW_systemClock_target.h"   // HW_systemClock_config_S

/* Public Function Declarations */
bool HW_systemClock_init(const HW_systemClock_config_S * const config);
