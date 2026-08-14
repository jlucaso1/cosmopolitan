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
#include "libc/dce.h"
#include "libc/intrin/maps.h"
#include "libc/intrin/weaken.h"
#include "libc/nt/process.h"
#include "libc/nt/runtime.h"
#include "libc/nt/struct/systeminfo.h"
#include "libc/nt/systeminfo.h"
#include "libc/runtime/internal.h"
#include "libc/runtime/runtime.h"
#include "libc/sysv/pib.h"
#include "libc/thread/tls.h"

/**
 * @fileoverview Bringing up the runtime inside somebody else's process.
 *
 * WinMain() assumes it owns the process: it allocates a stack at a fixed
 * address and switches to it, deduplicates the standard handles, installs
 * a vectored exception handler, invents a pid, and rewrites the console
 * modes. None of that is ours to do when we're a library the host loaded,
 * so this brings up only what the libc needs to answer calls.
 *
 * Thread local storage is the easy part here, unlike on System V: the tib
 * lives in a slot from TlsAlloc(), which by construction can't collide
 * with whatever the host is using.
 */

extern char cosmo_dll_hostos asm("__hostos");

void _init(void);

// weak, like cosmo2.c declares them: they come from the linker script,
// so no package defines them
typedef int init_f(int, char **, char **, unsigned long *);
extern init_f *__init_array_start[] __attribute__((__weak__));
extern init_f *__init_array_end[] __attribute__((__weak__));

static bool cosmo_dll_booted;
static char *cosmo_dll_argv[2];
static char *cosmo_dll_environ[1];

extern atomic_ulong __fake_process_signals;

/**
 * Starts Cosmopolitan Libc inside a host process.
 *
 * Must be __msabi, like everything a windows host calls into: cosmopolitan
 * is compiled for System V, where rsi and rdi are caller saved, while the
 * Microsoft convention has the callee preserve them. Getting that wrong
 * corrupts the caller's frame rather than failing outright.
 *
 * Safe to call more than once; only the first call does anything. Returns
 * true if the runtime is up.
 */
__msabi bool cosmo_dll_boot(void) {
  if (cosmo_dll_booted)
    return true;
  cosmo_dll_booted = true;

  cosmo_dll_hostos = _HOSTWINDOWS;
  __tls_enabled = false;

  struct NtSystemInfo si;
  GetSystemInfo(&si);
  __pagesize = si.dwPageSize;
  __gransize = si.dwAllocationGranularity;

  // the host owns the process, so take its identity rather than minting
  // one the way WinMain() does for a program it started
  struct CosmoPib *pib = __get_pib();
  pib->pid = GetCurrentProcessId();

  // __enable_tls() reads both of these, and WinMain is what normally
  // fills them in. Left null, the first is written through and the
  // second is walked.
  pib->sigpending = &__fake_process_signals;
  if (!environ)
    environ = cosmo_dll_environ;

  // decentralized init: system call dispatch, memory map, the lot. it
  // wants argc/argv/envp/auxv in r12 through r15, and a couple of the
  // fragments dereference argv, so give it something valid.
  cosmo_dll_argv[0] = (char *)"cosmo";
  register long r12 asm("r12") = 1;
  register char **r13 asm("r13") = cosmo_dll_argv;
  register char **r14 asm("r14") = cosmo_dll_argv + 1;
  register long r15 asm("r15") = 0;
  asm volatile("call\t_init"
               : "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
               : /* no inputs */
               : "rdi", "rsi", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11",
                 "memory", "cc");



  // these two live in cosmo.S, which nothing references in a library, so
  // the linker never pulls it in and their .init fragments never run
  __enable_tls();

  // And now the constructors, which nothing else is going to run: a PE
  // image has no equivalent of DT_INIT_ARRAY, and they assume a runtime
  // that is already up, several of them making system calls. malloc is
  // one of them, and until it runs the allocator is a null pointer.
  for (init_f **f = __init_array_start; f < __init_array_end; ++f)
    (*f)(1, cosmo_dll_argv, cosmo_dll_environ, 0);
  if (_weaken(__init_fds))
    _weaken(__init_fds)();

  return true;
}
