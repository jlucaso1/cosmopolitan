/*-*- mode:unix-assembly; indent-tabs-mode:t; tab-width:8; coding:utf-8     -*-│
│ vi: set noet ft=asm ts=8 sw=8 fenc=utf-8                                 :vi │
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
#include "ape/relocations.h"

//	A second file that publishes and asks for things.
//
//	Which is the point of it: the mach-o tables are built after the
//	link, out of what the linker sorted, so nothing has to be able to
//	see everything at once. The export directory windows reads still
//	does, since its ordinals come from a counter the assembler keeps,
//	so this half is only for the other platform.

#if SupportsXnu()

//	Reaching the program that opened us, which is what a plugin is
//	for. Flat, meaning look through everything already loaded rather
//	than at one library by name: the host is the host, whatever it
//	happens to be called.
	.import	__ape_host_add,"_cosmo_test_host_add",APE_IMPORT_FLAT

	.export	cosmo_dll_callhost

#endif /* SupportsXnu() */
