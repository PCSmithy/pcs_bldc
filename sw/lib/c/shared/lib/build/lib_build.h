#pragma once

// Named constants for the BUILD_TARGET preprocessor macro.
//
// The active target is selected by the toolchain file via a compile
// definition, e.g. -DBUILD_TARGET=BUILD_TARGET_STM32G4. Source code
// then branches with
//
//   #if (BUILD_TARGET == BUILD_TARGET_STM32G4)
//     ...
//   #elif (BUILD_TARGET == BUILD_TARGET_SIM)
//     ...
//   #endif
//
// so that the same file can define per-target struct contents, channel
// configurations, etc. without scattering #ifdefs across the codebase.

#define BUILD_TARGET_STM32G4 1
#define BUILD_TARGET_SIM     2

