/* pthr.c -- bionic-to-newlib pthread wrappers
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <switch.h>

#include "pthr.h"
#include "util.h"

// set in the trampoline when the game's renderThread_main starts; the GL
// ownership layer lets this thread keep the real context across parks, so its
// wait shims below must service handover in short slices instead of blocking
static __thread int tls_is_render_thread = 0;
static __thread int tls_capture_frame_sync = 0;
static __thread int tls_is_core_thread = 0;
static pthread_cond_t_bionic *frame_sync_cond;
static unsigned frame_sync_pending;
static int core_shutdown_requested;

int pthr_is_render_thread(void) {
  return tls_is_render_thread;
}

void pthr_capture_next_cond_wait_as_frame_sync(void) {
  tls_capture_frame_sync = 1;
}

static int frame_sync_match(pthread_cond_t_bionic *cond) {
  return cond && cond == __atomic_load_n(&frame_sync_cond, __ATOMIC_ACQUIRE);
}

static void frame_sync_capture(pthread_cond_t_bionic *cond) {
  if (!tls_capture_frame_sync) return;
  tls_capture_frame_sync = 0;
  pthread_cond_t_bionic *expected = NULL;
  __atomic_compare_exchange_n(&frame_sync_cond, &expected, cond, false,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// ---------------------------------------------------------------------------
// fake RW TLS: AArch64 -fstack-protector loads the canary relative to
// TPIDR_EL0, which libnx leaves pointing at read-only/zero storage. Each game
// thread gets a small zeroed block installed in TPIDR_EL0 so those loads (and
// any other bionic TLS-relative reads) land on valid, writable memory. The
// block is intentionally leaked: it must stay live for the thread's lifetime.
// ---------------------------------------------------------------------------

void pthr_install_fake_tls(void) {
  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);
}

void pthr_ensure_fake_tls(void) {
  if (armGetTlsRw() == NULL)
    pthr_install_fake_tls();
}

// ---------------------------------------------------------------------------
// lazy first-use initialization of game-owned sync objects: the hot path is
// one acquire-load of the magic word; only init and destroy take init_lock
// ---------------------------------------------------------------------------

#define PTHR_MUTEX_MAGIC 0x4D58544Du // "MTXM"
#define PTHR_COND_MAGIC  0x444E434Du // "CNDM"

static Mutex init_lock;

typedef struct BionicCondRecord {
  pthread_cond_t_bionic *owner;
  pthread_cond_t *real;
  struct BionicCondRecord *next;
} BionicCondRecord;

static BionicCondRecord *bionic_cond_head;

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_static_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
  // pairs with the release store below so real_ptr is visible once magic is
  if (__atomic_load_n(&mutex->magic, __ATOMIC_ACQUIRE) == PTHR_MUTEX_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) == PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock); // another thread won the first-use race
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else {
    // the kind word of a statically initialized bionic mutex (overlaps the
    // low half of real_ptr, which we haven't written yet)
    switch (*(int *)mutex) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));

  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    mutex->real_ptr = real;
    mutex->reserved = 0;
    __atomic_store_n(&mutex->magic, PTHR_MUTEX_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);

  }
  mutexUnlock(&init_lock);
  return ret;
}

static int cond_static_init(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) == PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }

  pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
  BionicCondRecord *record = malloc(sizeof(*record));
  int ret = (!real || !record) ? ENOMEM : pthread_cond_init(real, attr);

  if (ret == 0) {
    record->owner = cond;
    record->real = real;
    record->next = bionic_cond_head;
    bionic_cond_head = record;
    cond->real_ptr = real;
    cond->reserved = 0;
    __atomic_store_n(&cond->magic, PTHR_COND_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
    free(record);
  }
  mutexUnlock(&init_lock);
  return ret;
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr) {
  if (!cond) return EINVAL;

  /* Drastic starts several workers immediately before the parent initializes
   * their synchronization objects. A child can therefore reach cond_wait
   * first and create the host condition through cond_static_init(). Do not
   * erase that live object when the parent's explicit pthread_cond_init then
   * arrives: that leaves the child asleep on one host condition while the
   * parent signals a newly allocated second one at the same bionic address.
   *
   * Mutex initialization is already idempotent for this ordering. Make
   * conditions follow the same rule; cond_static_init overwrites every
   * wrapper field itself when the object genuinely is not initialized. */
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC) {
    return 0;
  }
  return cond_static_init(cond, attr);
}

// Keep the main emulation thread isolated and distribute workers across the
// remaining application cores.

static Mutex core_lock;
static int      core_count = 0;
static int      emulation_core = 0;
static unsigned hot_mask   = 0x1;   // all cores used for heavy emulation
static unsigned work_mask  = 0x1;   // hot_mask minus the emulation core
static int      bg_core    = -1;    // dedicated background core, or -1 = share work pool
static int      work_list[4];       // hot cores excluding emulation_core
static int      work_count = 0;
static unsigned work_rr = 0, bg_rr = 0;

static void core_init_once(void) {
  if (core_count)
    return;
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
    mask = 0x7; // fallback: cores 0,1,2 (typical app allotment)
  int core_list[4];
  for (int c = 0; c < 4; c++)
    if (mask & (1u << c))
      core_list[core_count++] = c;

  // Reserve the TOP core for background only when 4+ are granted; with <=3 we
  // need them all for the heavy emulation threads.
  int hot_count;
  if (core_count >= 4) {
    hot_count = core_count - 1;
    bg_core   = core_list[core_count - 1];
  } else {
    hot_count = core_count;
    bg_core   = -1;
  }
  hot_mask = 0;
  for (int i = 0; i < hot_count; i++)
    hot_mask |= (1u << core_list[i]);

  emulation_core = core_list[0];
  // Worker pool = the hot cores except the primary emulation core. If only one
  // hot core exists, all work unavoidably shares it.
  work_count = 0;
  for (int i = 1; i < hot_count; i++)
    work_list[work_count++] = core_list[i];
  if (work_count == 0)
    work_list[work_count++] = emulation_core;
  work_mask = (hot_count >= 2)
                  ? (hot_mask & ~(1u << emulation_core))
                  : hot_mask;


}

// The primary emulation thread benefits most from a stable, warm core.
void pthr_pin_emulation_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = emulation_core;
  const unsigned m = 1u << emulation_core;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);

}

// Heavy worker threads are round-robined over the work pool and can migrate
// within that pool without moving onto the primary emulation core.
static void assign_work_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = work_list[work_rr++ % (unsigned)work_count]; const unsigned m = work_mask;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
}

// background threads (audio, etc.): the dedicated bg core if we have one, else
// share the work pool. For threads our vendored code spawns directly through
// newlib pthread_create (the audio mixer/callback) -- they never pass through
// the trampoline, so pin them explicitly as well.
void pthr_pin_bg_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  int core; unsigned m;
  if (bg_core >= 0) { core = bg_core; m = 1u << bg_core; }
  else { core = work_list[bg_rr++ % (unsigned)work_count]; m = work_mask; }
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
}

// ---------------------------------------------------------------------------
// thread creation (installs the fake TLS before running game code)
// ---------------------------------------------------------------------------

typedef struct {
  void *(*start)(void *);
  void *arg;
} ThreadStart;

typedef struct CoreThreadRecord {
  pthread_t thread;
  struct CoreThreadRecord *next;
} CoreThreadRecord;

typedef struct DetachedThread {
  pthread_t thread;
  struct DetachedThread *next;
} DetachedThread;

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;
static DetachedThread *detach_head;
static DetachedThread *detach_tail;
static pthread_t detach_reaper;
static CoreThreadRecord *core_thread_head;
static unsigned active_core_threads;
static int detach_reaper_started;
static int detach_reaper_stop;

static void *detach_reaper_main(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&thread_lock);
    while (!detach_head && !detach_reaper_stop)
      pthread_cond_wait(&thread_cond, &thread_lock);
    DetachedThread *item = detach_head;
    if (item) {
      detach_head = item->next;
      if (!detach_head)
        detach_tail = NULL;
    } else if (detach_reaper_stop) {
      pthread_mutex_unlock(&thread_lock);
      return NULL;
    }
    pthread_mutex_unlock(&thread_lock);

    const int joined = pthread_join(item->thread, NULL);
    if (joined == 0) {
      pthread_mutex_lock(&thread_lock);
      CoreThreadRecord **link = &core_thread_head;
      while (*link) {
        if ((*link)->thread == item->thread) {
          CoreThreadRecord *record = *link;
          *link = record->next;
          free(record);
          break;
        }
        link = &(*link)->next;
      }
      pthread_mutex_unlock(&thread_lock);
    }
    free(item);
  }
}

static void core_thread_cleanup(void *unused) {
  (void)unused;
  if (!tls_is_core_thread)
    return;
  tls_is_core_thread = 0;

  // EGL ownership is thread-local and must be released before the renderer.
  extern void egl_gl_ownership_release(void);
  egl_gl_ownership_release();

  pthread_mutex_lock(&thread_lock);
  active_core_threads--;
  pthread_cond_broadcast(&thread_cond);
  pthread_mutex_unlock(&thread_lock);
}

static void *thread_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  pthr_install_fake_tls();
  // Keep emulator workers off the primary emulation and audio cores.
  assign_work_core();
  tls_is_core_thread = 1;
  void *ret;
  pthread_cleanup_push(core_thread_cleanup, NULL);
  ret = s.start(s.arg);
  pthread_cleanup_pop(1);
  return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  if (!thread || !start) {
    return EINVAL;
  }
  ThreadStart *s = malloc(sizeof(*s));
  CoreThreadRecord *record = malloc(sizeof(*record));
  if (!s || !record) {
    free(s);
    free(record);
    return ENOMEM;
  }
  s->start = start;
  s->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  // the engine's worker/render threads recurse deeply; give them headroom
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
  }

  pthread_mutex_lock(&thread_lock);
  active_core_threads++;
  pthread_mutex_unlock(&thread_lock);

  int ret = pthread_create(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (ret != 0) {
    pthread_mutex_lock(&thread_lock);
    active_core_threads--;
    pthread_cond_broadcast(&thread_cond);
    pthread_mutex_unlock(&thread_lock);
    free(s);
    free(record);
  } else {
    record->thread = *thread;
    pthread_mutex_lock(&thread_lock);
    record->next = core_thread_head;
    core_thread_head = record;
    pthread_mutex_unlock(&thread_lock);
  }
  return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) {
  const int ret = pthread_join(thread, value_ptr);
  if (ret == 0) {
    pthread_mutex_lock(&thread_lock);
    CoreThreadRecord **link = &core_thread_head;
    while (*link) {
      if ((*link)->thread == thread) {
        CoreThreadRecord *record = *link;
        *link = record->next;
        free(record);
        break;
      }
      link = &(*link)->next;
    }
    pthread_mutex_unlock(&thread_lock);
  }
  return ret;
}

int pthread_detach_soloader(pthread_t thread) {
  if (!thread)
    return ESRCH;

  DetachedThread *item = malloc(sizeof(*item));
  if (!item)
    return ENOMEM;
  item->thread = thread;
  item->next = NULL;

  pthread_mutex_lock(&thread_lock);
  if (!detach_reaper_started) {
    int ret = pthread_create(&detach_reaper, NULL, detach_reaper_main, NULL);
    if (ret != 0) {
      pthread_mutex_unlock(&thread_lock);
      free(item);
      return ret;
    }
    detach_reaper_started = 1;
  }
  if (detach_tail)
    detach_tail->next = item;
  else
    detach_head = item;
  detach_tail = item;
  pthread_cond_signal(&thread_cond);
  pthread_mutex_unlock(&thread_lock);
  return 0;
}

static void wake_all_bionic_conditions(void) {
  mutexLock(&init_lock);
  for (BionicCondRecord *record = bionic_cond_head; record;
       record = record->next)
    pthread_cond_broadcast(record->real);
  mutexUnlock(&init_lock);
}

static void destroy_remaining_bionic_conditions(void) {
  mutexLock(&init_lock);
  BionicCondRecord *record = bionic_cond_head;
  bionic_cond_head = NULL;
  __atomic_store_n(&frame_sync_pending, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&frame_sync_cond, NULL, __ATOMIC_RELEASE);
  mutexUnlock(&init_lock);

  /* Run this only after the core's C++ destructors. They get the first chance
   * to destroy their synchronization objects normally; the records left here
   * belong to Android-process singletons. Do not dereference owner storage at
   * this point because a destructor may already have freed a dynamic owner. */
  while (record) {
    BionicCondRecord *next = record->next;
    pthread_cond_destroy(record->real);
    free(record->real);
    free(record);
    record = next;
  }
}

void pthr_shutdown(void) {
  /* Android tears the process down with several DraStic service workers still
   * alive. One of them is intentionally an infinite command worker and has no
   * engine-side quit flag. Chainloading an NRO reuses the hbloader process, so
   * those threads must leave before libdrastic is unmapped. Ask core threads to
   * exit only when they reach their own condition-wait boundary, and keep
   * broadcasting during the handoff so a waiter cannot miss the first wake. */
  __atomic_store_n(&core_shutdown_requested, 1, __ATOMIC_RELEASE);
  for (;;) {
    wake_all_bionic_conditions();
    pthread_mutex_lock(&thread_lock);
    const unsigned active = active_core_threads;
    pthread_mutex_unlock(&thread_lock);
    if (!active)
      break;
    svcSleepThread(1000000ULL);
  }

  pthread_mutex_lock(&thread_lock);
  if (detach_reaper_started) {
    detach_reaper_stop = 1;
    pthread_cond_broadcast(&thread_cond);
  }
  pthread_mutex_unlock(&thread_lock);

  if (detach_reaper_started) {
    pthread_join(detach_reaper, NULL);
    detach_reaper_started = 0;
  }

  /* Core-owned joins removed their records already. Join the Android-process
   * workers stopped above so libnx can close their handles and reclaim stacks. */
  for (;;) {
    pthread_mutex_lock(&thread_lock);
    CoreThreadRecord *record = core_thread_head;
    if (record)
      core_thread_head = record->next;
    pthread_mutex_unlock(&thread_lock);
    if (!record)
      break;
    pthread_join(record->thread, NULL);
    free(record);
  }

}

void pthr_finalize(void) {
  destroy_remaining_bionic_conditions();
}
pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  // newlib on devkitA64 doesn't expose pthread_getschedparam; the game only
  // reads these to echo them back, so reporting a default schedule is fine
  (void)thread;
  if (policy) *policy = 0; // SCHED_OTHER
  if (param) param->sched_priority = 0;
  return 0;
}

enum {
  PTHREAD_ONCE_SOLOADER_INIT = 0,
  PTHREAD_ONCE_SOLOADER_RUNNING = 1,
  PTHREAD_ONCE_SOLOADER_DONE = 2,
};

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return EINVAL;

  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_DONE)
    return 0;

  pthread_mutex_lock(&once_lock);
  for (;;) {
    int state = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_ONCE_SOLOADER_DONE) {
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    if (state == PTHREAD_ONCE_SOLOADER_INIT) {
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_RUNNING, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&once_lock);

      (*init_routine)();

      pthread_mutex_lock(&once_lock);
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_DONE, __ATOMIC_RELEASE);
      pthread_cond_broadcast(&once_cond);
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    // A peer is still inside init_routine. POSIX requires callers to wait
    // until the initializer has completed; returning early exposes partially
    // initialized game singletons to other threads.
    pthread_mutex_unlock(&once_lock);
    extern void egl_gl_ownership_park(void);
    egl_gl_ownership_park();
    pthread_mutex_lock(&once_lock);
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_RUNNING)
      pthread_cond_wait(&once_cond, &once_lock);
  }
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  return mutex_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) != PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&mutex->magic, 0, __ATOMIC_RELEASE);
  pthread_mutex_t *real = mutex->real_ptr;
  mutex->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_mutex_destroy(real);
  free(real);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  // fast path: uncontended
  if (pthread_mutex_trylock(mutex->real_ptr) == 0)
    return 0;
  // contended: never block on a game mutex while holding the real GL context
  // (ABBA against the game's BeginCriticalSectionGL) -- park first
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_mutex_lock(mutex->real_ptr);
  // render thread still holding (park keeps it): the mutex holder might want
  // exactly that context, so poll with handover service instead of blocking
  for (;;) {
    if (pthread_mutex_trylock(mutex->real_ptr) == 0)
      return 0;
    struct timespec ts = { 0, 100 * 1000 }; // 0.1 ms
    nanosleep(&ts, NULL);
  }
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex || !mutex->real_ptr) return EINVAL;
  return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) != PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&cond->magic, 0, __ATOMIC_RELEASE);
  if (frame_sync_match(cond)) {
    __atomic_store_n(&frame_sync_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&frame_sync_cond, NULL, __ATOMIC_RELEASE);
  }
  pthread_cond_t *real = cond->real_ptr;
  cond->real_ptr = NULL;
  BionicCondRecord **link = &bionic_cond_head;
  while (*link) {
    if ((*link)->owner == cond) {
      BionicCondRecord *record = *link;
      *link = record->next;
      free(record);
      break;
    }
    link = &(*link)->next;
  }
  mutexUnlock(&init_lock);
  int ret = pthread_cond_destroy(real);
  free(real);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  if (frame_sync_match(cond))
    __atomic_store_n(&frame_sync_pending, 1, __ATOMIC_RELEASE);
  return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  if (frame_sync_match(cond))
    __atomic_store_n(&frame_sync_pending, 1, __ATOMIC_RELEASE);
  return pthread_cond_broadcast(cond->real_ptr);
}

// render-thread wait slice: while it parks holding the GL context, its waits
// poll for handover requests at this granularity
#define RENDER_WAIT_SLICE_NS (4 * 1000 * 1000)
#define FRAME_SYNC_WAIT_SLICE_NS (16 * 1000 * 1000)

static void timespec_add_ns(struct timespec *ts, long ns) {
  ts->tv_nsec += ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}

static int timespec_before(const struct timespec *a, const struct timespec *b) {
  if (a->tv_sec != b->tv_sec)
    return a->tv_sec < b->tv_sec;
  return a->tv_nsec < b->tv_nsec;
}

static void core_thread_exit_from_wait(pthread_mutex_t *mutex) {
  if (!tls_is_core_thread ||
      !__atomic_load_n(&core_shutdown_requested, __ATOMIC_ACQUIRE))
    return;

  /* pthread_cond_wait returns with the associated mutex locked. Release it so
   * another awakened worker can reach the same cooperative exit point. The
   * trampoline cleanup registered above performs accounting and EGL release. */
  pthread_mutex_unlock(mutex);
  pthread_exit(NULL);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  frame_sync_capture(cond);
  const int is_frame_sync = frame_sync_match(cond);
  core_thread_exit_from_wait(mutex->real_ptr);
  if (is_frame_sync &&
      __atomic_exchange_n(&frame_sync_pending, 0, __ATOMIC_ACQ_REL))
    return 0;
  // about to park: hand back the GL context so other threads can render
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  if (is_frame_sync) {
    /* The Android GLSurfaceView continuously redraws and POSIX permits
     * spurious condition wakeups.  Drastic nevertheless uses this condition
     * as an unlatched edge, so a transition can emit its final notification
     * just before waitScreen() parks.  Bound only this identified screen wait
     * to one display interval; on timeout the host safely redraws the latest
     * mutex-protected buffers and checks input again. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    timespec_add_ns(&deadline, FRAME_SYNC_WAIT_SLICE_NS);
    const int result = pthread_cond_timedwait(cond->real_ptr,
                                              mutex->real_ptr, &deadline);
    core_thread_exit_from_wait(mutex->real_ptr);
    if (result == 0)
      __atomic_store_n(&frame_sync_pending, 0, __ATOMIC_RELEASE);
    return result == ETIMEDOUT ? 0 : result;
  }
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context()) {
    const int result = pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
    core_thread_exit_from_wait(mutex->real_ptr);
    if (is_frame_sync && result == 0)
      __atomic_store_n(&frame_sync_pending, 0, __ATOMIC_RELEASE);
    return result;
  }
  // render thread still holding the GL binding: wait ONE short slice and
  // report a timeout as a spurious wakeup (legal; predicate loops re-check).
  // Never loop internally: the game may signal without holding the mutex,
  // and a signal landing between laps is lost forever -- the one-shot
  // level-ready signal then leaves the renderer asleep for good.
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, &ts);
  core_thread_exit_from_wait(mutex->real_ptr);
  if (is_frame_sync && r == 0)
    __atomic_store_n(&frame_sync_pending, 0, __ATOMIC_RELEASE);
  if (r == ETIMEDOUT) {
    return 0; // spurious wakeup (POSIX/bionic allow them)
  }
  return r;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  core_thread_exit_from_wait(mutex->real_ptr);
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !abstime || !egl_gl_thread_holds_context()) {
    const int result = pthread_cond_timedwait(cond->real_ptr,
                                              mutex->real_ptr, abstime);
    core_thread_exit_from_wait(mutex->real_ptr);
    return result;
  }
  // same single-slice spurious-wakeup scheme as pthread_cond_wait_soloader;
  // the caller's real deadline is only ever reported once it actually passes
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  const int final_slice = !timespec_before(&ts, abstime);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr,
                                 final_slice ? abstime : &ts);
  core_thread_exit_from_wait(mutex->real_ptr);
  if (r == ETIMEDOUT && !final_slice) {
    return 0; // spurious wakeup before the caller's deadline
  }
  return r;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}
