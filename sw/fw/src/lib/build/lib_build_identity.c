/* Includes */
// Deliberately not lib_build_config.h: an in-scope extern splits the DWARF
// into declaration + DW_AT_specification DIEs, which dwarf_map does not stitch.
#include "lib_build_identity.h"

/* Public Data Definitions */

// The ELF-side identity anchor: the single named copy of LIB_BUILD_IDENTITY,
// so the image file and the wire report the same object.
const char lib_build_identityString[] = LIB_BUILD_IDENTITY;
