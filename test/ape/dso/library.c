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
#include "libc/calls/calls.h"
#include "libc/mem/mem.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"

/**
 * @fileoverview The library half of the hosted shared object test.
 *
 * Where the PE and mach-o test only has to be reachable, this has to
 * work: the point is that cosmopolitan's libc answers correctly from
 * inside a process whose libc is somebody else's. So the calls here are
 * chosen to go somewhere. snprintf() is guest formatting, malloc() is the
 * guest heap, and getpid() is a guest system call whose right answer the
 * host already knows.
 */

int cosmo_dso_add(int a, int b) {
  return a + b;
}

/**
 * Reports the process this is running in, formatted by cosmopolitan.
 *
 * Returns the pid, which the host compares against its own.
 */
int cosmo_dso_probe(char *out, int size) {
  int pid = getpid();
  char *scratch = malloc(128);
  if (!scratch)
    return -1;
  snprintf(scratch, 128, "cosmo libc says pid=%d tid=%d", pid, gettid());
  strlcpy(out, scratch, size);
  free(scratch);
  return pid;
}
