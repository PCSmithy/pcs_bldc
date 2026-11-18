// LTO DWARF-shape fixture for dwarf_map: a static whose located DIE, in the
// link-time unit, names it only by a cross-unit DW_AT_abstract_origin back to
// the declaration in this file's unit.
//
// Built (from this directory, Arm GNU Toolchain 15.2.Rel1):
//
//   arm-none-eabi-gcc -g3 -O2 -flto -mcpu=cortex-m4 -mthumb -nostdlib \
//       -nostartfiles -Wl,-e,use_all -o lto_origin.elf lto_origin.c
//
// Only the linked ELF is checked in. `use_all` is the entry point, so the
// statics it touches survive the link.

#include <stdint.h>

typedef struct
{
    uint32_t count;
    float    gain;
    uint16_t raw[4];
} lto_module_S;

static lto_module_S lto_module_data;
static volatile uint32_t lto_probe_us;

void use_all(void)
{
    lto_module_data.count++;
    lto_module_data.raw[2] = (uint16_t) lto_module_data.count;
    lto_probe_us = lto_module_data.count;
}
