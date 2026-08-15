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

/**
 * @fileoverview The library half of the shared object test.
 *
 * Built with APE_DLL, this comes out as a PE dll on windows and a mach-o
 * dylib on macOS, which the host half then loads the way any native
 * program would.
 *
 * On windows every entry point has to be __msabi. Cosmopolitan is
 * compiled for System V, where rsi and rdi are caller saved, while the
 * Microsoft convention has the callee preserve them, so a host calling
 * straight in corrupts its own frame. Elsewhere the host is already
 * speaking System V and nothing needs to be said.
 */

#include "libc/calls/calls.h"
#include "libc/dce.h"
#include "libc/mem/mem.h"
#include "libc/nt/thunk/msabi.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"

#if SupportsWindows()
#define EXPORTED __attribute__((__ms_abi__))
#else
#define EXPORTED
#endif

/**
 * Adds two numbers, touching nothing.
 *
 * The point of this one is the file format alone: if the host can load
 * the image and reach this, the headers and export tables are right.
 */
EXPORTED int cosmo_dll_add(int a, int b) {
  return a + b;
}

#if SupportsWindows()
__msabi bool cosmo_dll_boot(void);
#endif

#if SupportsWindows()
/**
 * Brings the runtime up and returns, touching nothing else.
 */
EXPORTED int cosmo_dll_init(void) {
  return cosmo_dll_boot() ? 1 : 0;
}
#endif

/**
 * Where dyld puts a function belonging to whoever opened us.
 *
 * Not a library by name: flat, meaning whatever is already loaded, which
 * for a plugin is the program itself. The declaration is here and the
 * asking is in test/ape/dll/exports2.S, since nothing has to see both.
 */
int (*__ape_host_add)(int, int);

/**
 * Calls back into the host.
 *
 * The shape a native addon has: a library that reaches the program that
 * loaded it, and gets an answer only that program could give.
 */
EXPORTED int cosmo_dll_callhost(int a, int b) {
  if (!__ape_host_add)
    return -1;
  return __ape_host_add(a, b);
}

/**
 * Reports the process this is running in, formatted by cosmopolitan.
 *
 * Where the one above only has to be reachable, this has to work: it
 * asks the runtime for a system call, some formatting and a little heap.
 * Returns the pid, which the host compares against its own.
 */
EXPORTED int cosmo_dll_probe(char *out, int size) {
#if SupportsWindows()
  // there is nothing for a windows host to pass us, so the library
  // brings the runtime up on first use. elsewhere the host has to hand
  // over a thread local slot, so it calls cosmo_dylib_boot() itself.
  cosmo_dll_boot();
#endif
  int pid = getpid();
  char *scratch = malloc(128);
  if (!scratch)
    return -1;
  int tid = gettid();
  snprintf(scratch, 128, "cosmo libc says pid=%d tid=%d", pid, tid);
  strlcpy(out, scratch, size);
  free(scratch);
  return pid;
}

