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

#include "libc/dce.h"

#ifdef __x86_64__

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

#endif /* __x86_64__ */
