/* Build-109 Drastic JIT integration for Horizon's W^X CodeMemory model. */

#include <switch.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "drastic_jit.h"
#include "libc_shim.h"

#define DRASTIC_CODEGEN_OFFSET 0x0009558cu
#define DRASTIC_CODEGEN_RESUME (DRASTIC_CODEGEN_OFFSET + 0x10u)
#define DRASTIC_RESOLVE_OFFSET 0x00095e08u
#define DRASTIC_RESOLVE_RESUME (DRASTIC_RESOLVE_OFFSET + 0x10u)
#define DRASTIC_PRIMARY_LINK_OFFSET 0x00036198u
#define DRASTIC_PRIMARY_LINK_RESUME 0x000361f0u
#define DRASTIC_PRIMARY_VENEER      0x00036218u
#define DRASTIC_NESTED_LINK_OFFSET  0x00037c4cu
#define DRASTIC_NESTED_LINK_RESUME  0x00037bc0u
#define DRASTIC_NESTED_VENEER       0x000362a8u
#define DRASTIC_JIT_MANAGER    0x01420000u
#define DRASTIC_JIT_SIZE       0x01300000u
#define DRASTIC_CORE_BASE_SLOT 8328u
#define DRASTIC_DEFERRED_DIRTY_MAX 64u

uintptr_t drastic_jit_codegen_resume;
uintptr_t drastic_jit_resolve_resume;
uintptr_t drastic_jit_primary_link_resume;
uintptr_t drastic_jit_nested_link_resume;
intptr_t drastic_jit_alias_delta;

extern void *drastic_jit_codegen_original(void *block, void *core,
                                          uint32_t pc, uint32_t mode);
extern void drastic_jit_resolve_original(void *core);

typedef struct {
  uintptr_t before[6];
  uintptr_t after[6];
  unsigned reserved_before_active;
  int active;
  int reserved_before_dirty_count;
  unsigned dirty_count;
  uintptr_t dirty[DRASTIC_DEFERRED_DIRTY_MAX];
  int dirty_overflow;
} PendingJitPublish;

_Thread_local PendingJitPublish g_pending_publish;

_Static_assert(offsetof(PendingJitPublish, active) == 100,
               "JIT link hook active offset changed");
_Static_assert(offsetof(PendingJitPublish, dirty_count) == 108,
               "JIT link hook dirty-count offset changed");
_Static_assert(offsetof(PendingJitPublish, dirty) == 112,
               "JIT link hook dirty-list offset changed");
_Static_assert(offsetof(PendingJitPublish, dirty_overflow) == 624,
               "JIT link hook overflow offset changed");

static const uint32_t g_arena_bounds[6][2] = {
    {0x00000000u, 0x01000000u},
    {0x00000000u, 0x01000000u},
    {0x01000000u, 0x01100000u},
    {0x01000000u, 0x01100000u},
    {0x01100000u, DRASTIC_JIT_SIZE},
    {0x01100000u, DRASTIC_JIT_SIZE},
};

static int make_manager_writable(uintptr_t executable_base,
                                 uintptr_t manager, uintptr_t cursor[6],
                                 uintptr_t *writable_base_out) {
  uintptr_t writable_base = 0;
  if (!writable_base_out ||
      !jit_redirect_to_writable(executable_base, &writable_base) ||
      writable_base == executable_base) {
    return 0;
  }

  for (unsigned i = 0; i < 6; i++) {
    const uintptr_t value = ((const uintptr_t *)manager)[i];
    const uintptr_t rx_low = executable_base + g_arena_bounds[i][0];
    const uintptr_t rx_high = executable_base + g_arena_bounds[i][1];
    const uintptr_t rw_low = writable_base + g_arena_bounds[i][0];
    const uintptr_t rw_high = writable_base + g_arena_bounds[i][1];
    uintptr_t writable = value;
    if (value >= rx_low && value <= rx_high)
      writable = writable_base + (value - executable_base);
    else if (value < rw_low || value > rw_high) {
      return 0;
    }
    if (writable != value)
      ((uintptr_t *)manager)[i] = writable;
    cursor[i] = writable;
  }
  *writable_base_out = writable_base;
  return 1;
}

static int restore_manager_executable(uintptr_t executable_base,
                                      uintptr_t writable_base,
                                      uintptr_t manager,
                                      const uintptr_t cursor[6]) {
  for (unsigned i = 0; i < 6; i++) {
    const uintptr_t low = writable_base + g_arena_bounds[i][0];
    const uintptr_t high = writable_base + g_arena_bounds[i][1];
    if (cursor[i] < low || cursor[i] > high) {
      return 0;
    }
    ((uintptr_t *)manager)[i] = executable_base +
                                (cursor[i] - writable_base);
  }
  return 1;
}

static int defer_completed_block(const uintptr_t before[6],
                                 const uintptr_t after[6]) {
  PendingJitPublish *pending = &g_pending_publish;
  for (unsigned arena = 0; arena < 3; arena++) {
    const unsigned forward = arena * 2;
    const unsigned backward = forward + 1;
    if (after[forward] < before[forward] ||
        after[backward] > before[backward])
      return 0;
  }

  if (!pending->active) {
    memcpy(pending->before, before, sizeof(pending->before));
    memcpy(pending->after, after, sizeof(pending->after));
    pending->active = 1;
    return 1;
  }

  for (unsigned arena = 0; arena < 3; arena++) {
    const unsigned forward = arena * 2;
    const unsigned backward = forward + 1;
    if (before[forward] < pending->after[forward] ||
        before[backward] > pending->after[backward])
      return 0;
    if (after[forward] > pending->after[forward])
      pending->after[forward] = after[forward];
    if (after[backward] < pending->after[backward])
      pending->after[backward] = after[backward];
  }
  return 1;
}

static int publish_deferred_block(const PendingJitPublish *pending) {
  uintptr_t starts[6 + DRASTIC_DEFERRED_DIRTY_MAX];
  size_t sizes[6 + DRASTIC_DEFERRED_DIRTY_MAX];
  size_t count = 0;

  if (pending->dirty_overflow ||
      pending->dirty_count > DRASTIC_DEFERRED_DIRTY_MAX) {
    return 0;
  }

  for (unsigned arena = 0; arena < 3; arena++) {
    const unsigned forward = arena * 2;
    const unsigned backward = forward + 1;
    if (pending->after[forward] > pending->before[forward]) {
      starts[count] = pending->before[forward];
      sizes[count++] = pending->after[forward] - pending->before[forward];
    }
    if (pending->before[backward] > pending->after[backward]) {
      starts[count] = pending->after[backward];
      sizes[count++] = pending->before[backward] - pending->after[backward];
    }
  }
  for (unsigned i = 0; i < pending->dirty_count; i++) {
    starts[count] = pending->dirty[i];
    sizes[count++] = sizeof(uint32_t);
  }

  if (!count)
    return 1;
  if (!jit_redirect_publish_writable(starts, sizes, count)) {
    return 0;
  }
  return 1;
}

__attribute__((noinline))
void drastic_jit_resolve_hook(void *core) {
  drastic_jit_resolve_original(core);

  PendingJitPublish *pending = &g_pending_publish;
  if (!pending->active) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }

  if (!publish_deferred_block(pending))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));

  memset(pending, 0, sizeof(*pending));
}

__attribute__((noinline))
void *drastic_jit_codegen_hook(void *block, void *core, uint32_t pc,
                               uint32_t mode) {
  const uintptr_t base = *(const uintptr_t *)((const char *)core +
                                               DRASTIC_CORE_BASE_SLOT);
  const uintptr_t manager = base + DRASTIC_JIT_MANAGER;
  uintptr_t writable_base;
  uintptr_t before[6];
  uintptr_t after[6];

  if (!base || !make_manager_writable(base, manager, before,
                                      &writable_base)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }

  const intptr_t alias_delta = (intptr_t)writable_base - (intptr_t)base;
  const intptr_t installed_delta = __atomic_load_n(
      &drastic_jit_alias_delta, __ATOMIC_ACQUIRE);
  if ((installed_delta && installed_delta != alias_delta) || !alias_delta) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }
  if (!installed_delta)
    __atomic_store_n(&drastic_jit_alias_delta, alias_delta,
                     __ATOMIC_RELEASE);

  void *writable_entry = drastic_jit_codegen_original(block, core, pc, mode);
  memcpy(after, (const void *)manager, sizeof(after));
  if (!restore_manager_executable(base, writable_base, manager, after)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }
  if (!defer_completed_block(before, after)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }

  if (!writable_entry)
    return NULL;

  const uintptr_t entry = (uintptr_t)writable_entry;
  if (entry < writable_base ||
      entry - writable_base >= DRASTIC_JIT_SIZE) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  }
  return (void *)(base + (entry - writable_base));
}

static uint32_t branch_to(uintptr_t from, uintptr_t to) {
  return 0x14000000u |
         ((uint32_t)(((intptr_t)to - (intptr_t)from) >> 2) & 0x03ffffffu);
}

static int patch_word(void *image, size_t image_size, size_t offset,
                      uint32_t expected, uint32_t replacement) {
  if (offset > image_size || sizeof(uint32_t) > image_size - offset)
    return 0;
  uint32_t *word = (uint32_t *)((char *)image + offset);
  if (*word != expected)
    return 0;
  *word = replacement;
  return 1;
}

int drastic_jit_install(so_module *mod) {
  static const uint32_t codegen_prologue[4] = {
      0xa9ba6ffcu, 0xa90167fau, 0xa9025ff8u, 0xa90357f6u,
  };
  static const uint32_t resolve_prologue[4] = {
      0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu, 0x910083fdu,
  };
  static const uint32_t primary_veneer_words[4] = {
      0xd53b002au, 0xa940350cu, 0x5280008bu, 0x12000d4eu,
  };
  static const uint32_t nested_veneer_words[4] = {
      0xd53b002au, 0xa940350cu, 0x5280008bu, 0x12000d4eu,
  };
  static const struct {
    uint32_t offset;
    uint32_t expected;
    uint32_t target;
  } old_publishers[] = {
      {0x36214u, 0x54000400u, 0x36294u},
      {0x362a4u, 0x54000400u, 0x36324u},
      {0x3632cu, 0x54000400u, 0x363acu},
      {0x363b4u, 0x54000400u, 0x36434u},
      {0x3643cu, 0x54000400u, 0x364bcu},
      {0x364c4u, 0x54000440u, 0x3654cu},
  };

  if (!mod || !mod->load_base || !mod->load_virtbase ||
      DRASTIC_CODEGEN_OFFSET + sizeof(codegen_prologue) > mod->load_size ||
      DRASTIC_RESOLVE_OFFSET + sizeof(resolve_prologue) > mod->load_size ||
      DRASTIC_PRIMARY_VENEER + sizeof(primary_veneer_words) > mod->load_size ||
      DRASTIC_NESTED_VENEER + sizeof(nested_veneer_words) > mod->load_size ||
      memcmp((const char *)mod->load_base + DRASTIC_CODEGEN_OFFSET,
             codegen_prologue, sizeof(codegen_prologue)) != 0 ||
      memcmp((const char *)mod->load_base + DRASTIC_RESOLVE_OFFSET,
             resolve_prologue, sizeof(resolve_prologue)) != 0 ||
      memcmp((const char *)mod->load_base + DRASTIC_PRIMARY_VENEER,
             primary_veneer_words, sizeof(primary_veneer_words)) != 0 ||
      memcmp((const char *)mod->load_base + DRASTIC_NESTED_VENEER,
             nested_veneer_words, sizeof(nested_veneer_words)) != 0)
    return 0;

  for (unsigned i = 0; i < sizeof(old_publishers) / sizeof(*old_publishers);
       i++) {
    const uintptr_t from = (uintptr_t)mod->load_virtbase +
                           old_publishers[i].offset;
    const uintptr_t to = (uintptr_t)mod->load_virtbase +
                         old_publishers[i].target;
    if (!patch_word(mod->load_base, mod->load_size,
                    old_publishers[i].offset, old_publishers[i].expected,
                    branch_to(from, to)))
      return 0;
  }

  const uintptr_t primary_link_from = (uintptr_t)mod->load_virtbase +
                                      DRASTIC_PRIMARY_LINK_OFFSET;
  const uintptr_t primary_veneer = (uintptr_t)mod->load_virtbase +
                                   DRASTIC_PRIMARY_VENEER;
  const uintptr_t nested_link_from = (uintptr_t)mod->load_virtbase +
                                     DRASTIC_NESTED_LINK_OFFSET;
  const uintptr_t nested_veneer = (uintptr_t)mod->load_virtbase +
                                  DRASTIC_NESTED_VENEER;
  if (!patch_word(mod->load_base, mod->load_size,
                  DRASTIC_PRIMARY_LINK_OFFSET, 0xb85fc008u,
                  branch_to(primary_link_from, primary_veneer)) ||
      !patch_word(mod->load_base, mod->load_size,
                  DRASTIC_NESTED_LINK_OFFSET, 0xb85fc008u,
                  branch_to(nested_link_from, nested_veneer)))
    return 0;

  uint32_t hook[4] = {
      0x58000050u, /* ldr x16, .+8 */
      0xd61f0200u, /* br x16 */
      (uint32_t)(uintptr_t)drastic_jit_codegen_hook,
      (uint32_t)((uintptr_t)drastic_jit_codegen_hook >> 32),
  };
  memcpy((char *)mod->load_base + DRASTIC_CODEGEN_OFFSET, hook, sizeof(hook));
  uint32_t resolve_hook[4] = {
      0x58000050u, /* ldr x16, .+8 */
      0xd61f0200u, /* br x16 */
      (uint32_t)(uintptr_t)drastic_jit_resolve_hook,
      (uint32_t)((uintptr_t)drastic_jit_resolve_hook >> 32),
  };
  memcpy((char *)mod->load_base + DRASTIC_RESOLVE_OFFSET,
         resolve_hook, sizeof(resolve_hook));
  uint32_t primary_link_hook[4] = {
      0x58000050u, /* ldr x16, .+8 */
      0xd61f0200u, /* br x16 */
      (uint32_t)(uintptr_t)drastic_jit_primary_link_store,
      (uint32_t)((uintptr_t)drastic_jit_primary_link_store >> 32),
  };
  uint32_t nested_link_hook[4] = {
      0x58000050u, /* ldr x16, .+8 */
      0xd61f0200u, /* br x16 */
      (uint32_t)(uintptr_t)drastic_jit_nested_link_store,
      (uint32_t)((uintptr_t)drastic_jit_nested_link_store >> 32),
  };
  memcpy((char *)mod->load_base + DRASTIC_PRIMARY_VENEER,
         primary_link_hook, sizeof(primary_link_hook));
  memcpy((char *)mod->load_base + DRASTIC_NESTED_VENEER,
         nested_link_hook, sizeof(nested_link_hook));
  drastic_jit_codegen_resume = (uintptr_t)mod->load_virtbase +
                               DRASTIC_CODEGEN_RESUME;
  drastic_jit_resolve_resume = (uintptr_t)mod->load_virtbase +
                               DRASTIC_RESOLVE_RESUME;
  drastic_jit_primary_link_resume = (uintptr_t)mod->load_virtbase +
                                    DRASTIC_PRIMARY_LINK_RESUME;
  drastic_jit_nested_link_resume = (uintptr_t)mod->load_virtbase +
                                   DRASTIC_NESTED_LINK_RESUME;
  return 1;
}
