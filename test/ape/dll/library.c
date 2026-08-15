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
 * Reports the process this is running in, formatted by cosmopolitan.
 *
 * Where the one above only has to be reachable, this has to work: it
 * asks the runtime for a system call, some formatting and a little heap.
 * Returns the pid, which the host compares against its own.
 */
void __cosmo_boot_trace(const char *, unsigned long);

EXPORTED int cosmo_dll_probe(char *out, int size) {
  __cosmo_boot_trace("prb", (unsigned long)out);
#if SupportsWindows()
  // there is nothing for a windows host to pass us, so the library
  // brings the runtime up on first use. elsewhere the host has to hand
  // over a thread local slot, so it calls cosmo_dylib_boot() itself.
  cosmo_dll_boot();
#endif
  int pid = getpid();
  __cosmo_boot_trace("pid", pid);
  char *scratch = malloc(128);
  __cosmo_boot_trace("mal", (unsigned long)scratch);
  if (!scratch)
    return -1;
  int tid = gettid();
  __cosmo_boot_trace("tid", tid);
  snprintf(scratch, 128, "cosmo libc says pid=%d tid=%d", pid, tid);
  __cosmo_boot_trace("snp", 0);
  strlcpy(out, scratch, size);
  __cosmo_boot_trace("cpy", 0);
  free(scratch);
  return pid;
}

