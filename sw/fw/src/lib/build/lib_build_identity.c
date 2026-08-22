/* Includes */
// Only the generated macro header — deliberately NOT lib_build_config.h:
// with the extern declaration in scope, GCC splits the variable's DWARF into
// a declaration DIE plus a DW_AT_specification definition, which address-map
// readers (dwarf_map) don't stitch. A self-contained definition emits one
// complete DIE.
#include "lib_build_identity.h"

/* Public Data Definitions */

// The ELF-side identity anchor (declared in lib_build_config.h): the single
// named copy of LIB_BUILD_IDENTITY, so the image file and the wire report
// the same object.
const char lib_build_identityString[] = LIB_BUILD_IDENTITY;
