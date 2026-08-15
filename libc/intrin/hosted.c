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
#include "libc/thread/tls.h"

/**
 * @fileoverview What the runtime knows about not owning the process.
 *
 * These live down here rather than with the code that sets them because
 * the startup, the memory manager, the signal machinery and the thread
 * library all have an opinion about being a guest, and this is the one
 * package all of them already depend on.
 */

/**
 * Set when cosmopolitan is a library inside somebody else's process.
 *
 * What it turns off is everything that assumes the process is ours:
 * installing a first-in-line exception handler, replacing the console
 * control handler, starting a worker thread, and working out where the
 * main stack is by reading from wherever the environment happens to
 * live.
 */
bool __cosmo_hosted;

/**
 * The thread the runtime was brought up on, if it was hosted.
 *
 * Threads the host made arrive with nothing in their thread local slot,
 * and building one for them starts by borrowing this.
 */
struct CosmoTib *__cosmo_hosted_main_tib;
