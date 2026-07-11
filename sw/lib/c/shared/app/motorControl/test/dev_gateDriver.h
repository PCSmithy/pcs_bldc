#pragma once

// Boundary double of dev_gateDriver — only the surface app_motorControl uses
// (the operational gate). Implementation + controls in mock_dev_gateDriver.
// Rationale: forcing "operational" through the real driver would mean feeding
// STSPIN32G4 STATUS-register bytes over a mocked I2C; the double lets a test
// just set the operational flag.

#include "lib_types.h"
#include "dev_gateDriver_channels.h"

bool dev_gateDriver_isOperational(dev_gateDriver_channel_E channel);
