/* crash.c -- libnx CPU exception handler.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stddef.h>
#include <string.h>

#include "libc_shim.h"

#define EXCEPTION_SLOT_COUNT 8
#define EXCEPTION_SLOT_SIZE  0x400
#define EXCEPTION_STACK_SIZE 0x10000

typedef struct {
  ThreadExceptionDump dump;
  u8 padding[0x3d0 - sizeof(ThreadExceptionDump)];
  u64 original_x0;
  u64 original_pc;
  u32 deferred;
  u32 result;
  void *frame;
  u32 index;
  u8 tail[EXCEPTION_SLOT_SIZE - 0x3f4];
} ExceptionSlot;

_Static_assert(sizeof(ExceptionSlot) == EXCEPTION_SLOT_SIZE, "invalid exception slot size");
_Static_assert(offsetof(ExceptionSlot, original_x0) == 0x3d0, "invalid original_x0 offset");
_Static_assert(offsetof(ExceptionSlot, original_pc) == 0x3d8, "invalid original_pc offset");
_Static_assert(offsetof(ExceptionSlot, deferred) == 0x3e0, "invalid deferred offset");
_Static_assert(offsetof(ExceptionSlot, result) == 0x3e4, "invalid result offset");
_Static_assert(offsetof(ExceptionSlot, frame) == 0x3e8, "invalid frame offset");
_Static_assert(offsetof(ExceptionSlot, index) == 0x3f0, "invalid index offset");

/* The backing array is emitted by exc_entry.s. Keeping this large override
 * out of an LTO object avoids a false size conflict with libnx's weak 1 KiB
 * default stack while preserving eight independent 64 KiB fault stacks. */
u64 __nx_exception_stack_size = EXCEPTION_STACK_SIZE;
ExceptionSlot g_exc_slots[EXCEPTION_SLOT_COUNT];
volatile u32 g_exc_slot_mask;
static volatile int g_crashing;

extern void fastmem_fault_trampoline(void);
extern void fastmem_fault_resume_marker(void);

static ExceptionSlot *exception_slot_from_dump(ThreadExceptionDump *dump) {
  const uintptr_t address = (uintptr_t)dump;
  const uintptr_t base = (uintptr_t)g_exc_slots;
  if (address < base || address - base >= sizeof(g_exc_slots) ||
      ((address - base) & (EXCEPTION_SLOT_SIZE - 1)))
    return NULL;
  return (ExceptionSlot *)dump;
}

static void exception_slot_release(ExceptionSlot *slot) {
  if (slot && slot->index < EXCEPTION_SLOT_COUNT)
    __atomic_fetch_and(&g_exc_slot_mask, ~(1u << slot->index), __ATOMIC_RELEASE);
}

int fastmem_run_deferred_fault(ThreadExceptionDump *dump) {
  ExceptionSlot *slot = exception_slot_from_dump(dump);
  if (!slot)
    return 0;
  const int handled = fastmem_dispatch_fault((uintptr_t)slot->original_pc,
                                             (uintptr_t)slot->dump.far.x);
  slot->dump.cpu_gprs[0].x = slot->original_x0;
  slot->dump.pc.x = slot->original_pc;
  return handled;
}

int crash_in_progress(void) {
  return __atomic_load_n(&g_crashing, __ATOMIC_ACQUIRE);
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  if ((uintptr_t)ctx->pc.x >= (uintptr_t)fastmem_fault_resume_marker &&
      (uintptr_t)ctx->pc.x < (uintptr_t)fastmem_fault_resume_marker + 8) {
    ExceptionSlot *original = exception_slot_from_dump(
        (ThreadExceptionDump *)(uintptr_t)ctx->cpu_gprs[19].x);
    if (original && original->deferred) {
      const int handled = original->result != 0;
      memcpy(ctx, &original->dump, sizeof(*ctx));
      exception_slot_release(original);
      if (handled)
        return;
      goto real_crash;
    }
  }

  {
    const unsigned ec = (ctx->esr >> 26) & 0x3f;
    if ((ec == 0x24 || ec == 0x25) &&
        fastmem_fault_can_dispatch((uintptr_t)ctx->pc.x, (uintptr_t)ctx->far.x)) {
      ExceptionSlot *slot = exception_slot_from_dump(ctx);
      if (slot) {
        slot->original_x0 = ctx->cpu_gprs[0].x;
        slot->original_pc = ctx->pc.x;
        slot->deferred = 1;
        slot->result = 0;
        ctx->cpu_gprs[0].x = (uintptr_t)ctx;
        ctx->pc.x = (uintptr_t)fastmem_fault_trampoline;
        return;
      }
    }
  }

real_crash:
  if (__atomic_exchange_n(&g_crashing, 1, __ATOMIC_ACQ_REL)) {
    for (;;) svcSleepThread(1000000000ULL);
  }
  (void)ctx;
  svcExitProcess();
}
