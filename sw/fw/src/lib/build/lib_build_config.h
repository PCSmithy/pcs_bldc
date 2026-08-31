#pragma once

/* Includes */
#include "lib_types.h"
// [impl->fw~obs_identity_001~1] LIB_BUILD_IDENTITY: commit hash of the
// built source, +diff hash when the tree was dirty. Regenerated every
// build by generate_identity.cmake.
#include "lib_build_identity.h"

/* Public Data Declarations */

// The identity as a named, DWARF-visible object: the board serves it over
// the protocol, and a host reads it straight out of an image file (the
// app's identity gate compares the two).
extern const char lib_build_identityString[];
