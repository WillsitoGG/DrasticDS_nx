#ifndef DRASTIC_JIT_H
#define DRASTIC_JIT_H

#include "so_util.h"

/* Drastic encodes AArch64 PC-relative references while writing a block.  An
 * alias separated by a multiple of 8 GiB has identical low 33 address bits,
 * which preserves ADR/ADRP, literal-load and branch encodings. */
#define DRASTIC_JIT_ALIAS_CONGRUENCE UINT64_C(0x200000000)

int drastic_jit_install(so_module *mod);

void drastic_jit_primary_link_store(void);
void drastic_jit_nested_link_store(void);

#endif
