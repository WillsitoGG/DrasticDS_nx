/* Hardware-compatibility fixes for the supported Drastic ARM64 core. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "drastic_compat.h"

/* r2.6.0.4a build 109 regressed one byte-write path for AUXSPICNT.  It stores
 * bit 7 even though that bit is the read-only SPI busy flag.  Dementium II
 * writes the register and then polls the flag, so the incorrect stored value
 * leaves the game in an infinite loop at boot.
 *
 * The older core masks bit 7 before storing.  Reuse a nearby two-instruction
 * dispatch stub for that operation, and redirect the stub's original DIV
 * register entries to an equivalent existing handler.  The latter also masks
 * reserved bit 7, preserving the original DIV side effect and valid values. */
#define WRITE8_DISPATCH_BASE       0x0010a6e8u
#define WRITE8_AUXSPICNT_LOW       0x01a0u
#define WRITE8_DIVCNT_LOW          0x0280u
#define WRITE8_DIV_RESULT_FIRST    0x0290u
#define WRITE8_DIV_RESULT_LAST     0x029fu

#define AUX_MASK_STUB_OFFSET       0x000235b0u
#define DIV_MASK_STUB_OFFSET       0x00023850u

#define DISPATCH_COMMON_STORE      0x019bu
#define DISPATCH_AUX_MASK_STUB     0x002fu
#define DISPATCH_DIV_MASK_STUB     0x00d7u

#define ARM64_STRB_WZR_X26_88      0x3901635fu
#define ARM64_AND_W20_7F           0x12001a94u
#define ARM64_B_COMMON_FROM_AUX    0x1400016bu
#define ARM64_B_COMMON_FROM_DIV    0x140000c2u

static int range_valid(const so_module *mod, size_t offset, size_t size) {
  return mod && mod->load_base && offset <= mod->load_size &&
         size <= mod->load_size - offset;
}

static uint32_t read_u32(const void *base, size_t offset) {
  uint32_t value;
  memcpy(&value, (const char *)base + offset, sizeof(value));
  return value;
}

static uint16_t read_u16(const void *base, size_t offset) {
  uint16_t value;
  memcpy(&value, (const char *)base + offset, sizeof(value));
  return value;
}

static void write_u32(void *base, size_t offset, uint32_t value) {
  memcpy((char *)base + offset, &value, sizeof(value));
}

static void write_u16(void *base, size_t offset, uint16_t value) {
  memcpy((char *)base + offset, &value, sizeof(value));
}

static size_t dispatch_offset(unsigned io_offset) {
  return WRITE8_DISPATCH_BASE + io_offset * sizeof(uint16_t);
}

int drastic_compat_install(so_module *mod) {
  const size_t table_end = dispatch_offset(WRITE8_DIV_RESULT_LAST) +
                           sizeof(uint16_t);
  if (!range_valid(mod, AUX_MASK_STUB_OFFSET, 2 * sizeof(uint32_t)) ||
      !range_valid(mod, DIV_MASK_STUB_OFFSET, 3 * sizeof(uint32_t)) ||
      !range_valid(mod, WRITE8_DISPATCH_BASE,
                   table_end - WRITE8_DISPATCH_BASE))
    return 0;

  /* Validate the complete code and table layout before changing any byte. */
  if (read_u32(mod->load_base, AUX_MASK_STUB_OFFSET) !=
          ARM64_STRB_WZR_X26_88 ||
      read_u32(mod->load_base, AUX_MASK_STUB_OFFSET + 4) !=
          ARM64_B_COMMON_FROM_AUX ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET) != ARM64_AND_W20_7F ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET + 4) !=
          ARM64_STRB_WZR_X26_88 ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET + 8) !=
          ARM64_B_COMMON_FROM_DIV ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_AUXSPICNT_LOW)) !=
          DISPATCH_COMMON_STORE ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW)) !=
          DISPATCH_AUX_MASK_STUB ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW + 1)) !=
          DISPATCH_DIV_MASK_STUB)
    return 0;

  for (unsigned offset = WRITE8_DIV_RESULT_FIRST;
       offset <= WRITE8_DIV_RESULT_LAST; offset++) {
    if (read_u16(mod->load_base, dispatch_offset(offset)) !=
        DISPATCH_AUX_MASK_STUB)
      return 0;
  }

  write_u32(mod->load_base, AUX_MASK_STUB_OFFSET, ARM64_AND_W20_7F);
  write_u16(mod->load_base, dispatch_offset(WRITE8_AUXSPICNT_LOW),
            DISPATCH_AUX_MASK_STUB);
  write_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW),
            DISPATCH_DIV_MASK_STUB);
  for (unsigned offset = WRITE8_DIV_RESULT_FIRST;
       offset <= WRITE8_DIV_RESULT_LAST; offset++) {
    write_u16(mod->load_base, dispatch_offset(offset),
              DISPATCH_DIV_MASK_STUB);
  }
  return 1;
}
