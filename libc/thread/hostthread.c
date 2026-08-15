/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2026 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/atomic.h"
#include "libc/calls/syscall-sysv.internal.h"
#include "libc/dce.h"
#include "libc/intrin/atomic.h"
#include "libc/mem/mem.h"
#include "libc/nt/files.h"
#include "libc/nt/runtime.h"
#include "libc/nt/thread.h"
#include "libc/thread/tls.h"
#include "third_party/dlmalloc/dlmalloc.h"

/**
 * @fileoverview Lending the runtime to threads the host made.
 *
 * This is the other half of libc/runtime/windllboot.c and dylibboot.c,
 * and it lives here rather than there because building a thread
 * information block is the thread library's business, and libc/runtime
 * doesn't depend on it. A weak reference wouldn't do: nothing else in a
 * library pulls the thread machinery in, so there would be nothing to
 * point at.
 */

extern struct CosmoTib *__cosmo_hosted_main_tib;

extern long __cosmo_hosted_tls_key;
int __cosmo_hosted_tid(void);
void __cosmo_boot_trace(const char *, unsigned long);

#ifdef __x86_64__
struct CosmoTib *__get_tls_rax(void);
#define __get_tls_here() __get_tls_rax()
#else
// x28 always holds something, since a thunk fills it in on the way in
// with the block the startup used if the slot is empty, so what says
// whether this thread has a tib of its own is the slot itself
static struct CosmoTib *__get_tls_here(void) {
  if (!__cosmo_hosted_tls_key)
    return 0;
  char *base;
  asm("mrs\t%0,tpidrro_el0" : "=r"(base));
  // what the slot holds is what x28 holds, which is past the end of the
  // block, the same way __get_tls() reads the register
  struct CosmoTib *past =
      ((struct CosmoTib **)((unsigned long)base &
                            ~7ul))[__cosmo_hosted_tls_key];
  return past ? past - 1 : 0;
}
#endif

/**
 * Adopts a thread the host created.
 *
 * Only the thread that booted the runtime has a tib. Any other one
 * arrives with an empty slot, and the first thing to want thread local
 * storage, which is to say malloc or errno or stdio, reads a null
 * pointer. Cosmopolitan does this itself for the threads it creates,
 * where pthread_create() builds the tib and the clone installs it.
 *
 * A host calling in from a pool of its own has to do this on each of
 * those threads, and the teardown before letting one go. Safe to call
 * more than once on the same thread.
 */
static int cosmo_hosted_thread_init(void) {
  if (!__cosmo_hosted_main_tib)
    return -1;
  if (__get_tls_here())
    return 0;  // already adopted

  // _mktls() copies ftrace, strace and the signal mask off the current
  // tib, so it needs a valid one installed; lend it the main thread's
  __cosmo_boot_trace("lend", (unsigned long)__cosmo_hosted_main_tib);
  __set_tls(__cosmo_hosted_main_tib);
  struct CosmoTib *tib;
  __cosmo_boot_trace("mk", 0);
  char *tls = _mktls(&tib);
  __cosmo_boot_trace("mk2", (unsigned long)tls);
  if (!tls) {
    __set_tls(0);
    return -1;
  }
  tib->tib_malloc = tmspace_acquire();
  __cosmo_boot_trace("heap", (unsigned long)tib->tib_malloc);

  // what __enable_tls() gives the main thread, and what anything that
  // suspends or signals a thread goes through
  if (IsWindows()) {
    intptr_t hand;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &hand, 0, false,
                    kNtDuplicateSameAccess);
    atomic_init(&tib->tib_syshand, hand);
  }

  tib->tib_keys_dynamic = (void *)tls;  // so the teardown can free it

  // not gettid(), which answers out of the tib, and the tib installed
  // just now is still the main thread's
  int tid = __cosmo_hosted_tid();
  if (!tid)
    tid = sys_gettid();
  atomic_init(&tib->tib_ptid, tid);
  atomic_init(&tib->tib_ctid, tid);
  __cosmo_boot_trace("adop", (unsigned long)tib);
  __set_tls(tib);
  __cosmo_boot_trace("adp2", 0);
  return 0;
}

/**
 * Hands back what the adoption took.
 */
static void cosmo_hosted_thread_fini(void) {
  __cosmo_boot_trace("fini", 0);
  struct CosmoTib *tib = __get_tls_here();
  if (!tib || tib == __cosmo_hosted_main_tib)
    return;
  // Order matters. Giving the thread pointer back first would leave
  // free() with nowhere to look for the heap it's returning memory to,
  // since that is kept in the block being pointed at.
  void *tls = tib->tib_keys_dynamic;
  void *heap = tib->tib_malloc;
  intptr_t hand = atomic_load_explicit(&tib->tib_syshand, memory_order_relaxed);
  __cosmo_boot_trace("ftls", (unsigned long)tls);
  free(tls);
  __cosmo_boot_trace("frel", (unsigned long)heap);
  tmspace_release(heap);
  __cosmo_boot_trace("fclr", 0);
  __set_tls(0);
  __cosmo_boot_trace("fdon", 0);
  if (IsWindows() && hand)
    CloseHandle(hand);
}

#if SupportsWindows()
/**
 * Adopts a thread, for a host that speaks the microsoft convention.
 */
__msabi int cosmo_dll_thread_init(void) {
  return cosmo_hosted_thread_init();
}

/**
 * Hands back what cosmo_dll_thread_init() took.
 */
__msabi void cosmo_dll_thread_fini(void) {
  cosmo_hosted_thread_fini();
}
#endif

#if SupportsXnu()
/**
 * Adopts a thread the mach-o host made.
 */
int cosmo_dylib_thread_init(void) {
  return cosmo_hosted_thread_init();
}

/**
 * Hands back what cosmo_dylib_thread_init() took.
 */
void cosmo_dylib_thread_fini(void) {
  cosmo_hosted_thread_fini();
}
#endif
