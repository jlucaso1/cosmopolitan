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
#include "libc/macros.h"

#ifdef __aarch64__

//	Handing the thread pointer back and forth with the host.
//
//	Cosmopolitan keeps its thread information block in x28, because
//	Apple won't let anyone else have tpidr_el0. A host compiled the
//	ordinary way keeps whatever it likes in x28 and expects to find it
//	there afterwards, so every way in has to save what was there,
//	install ours, and put it back on the way out.
//
//	Ours comes from the slot the host gave us, which is a pthread key,
//	and on this architecture the storage for key k is at k*8 from the
//	thread pointer. Before there is a key there is the block below,
//	which is enough for the startup to get as far as making a real one.

	.bss
	.balign	16
__ape_boot_tls:
	.zero	1024 + 512			// tls image and the tib
__ape_boot_tls_end:
	.endobj	__ape_boot_tls,globl,hidden

//	Puts the guest's thread pointer in x28.
//
//	Touches x9 and x10 only, so a thunk can call it without disturbing
//	anything an exported function was passed.
	.text
__ape_load_tls:
	mrs	x9,tpidrro_el0
	and	x9,x9,#~7
	adrp	x10,__cosmo_hosted_tls_key
	add	x10,x10,#:lo12:__cosmo_hosted_tls_key
	ldr	x10,[x10]
	cbz	x10,1f
	ldr	x28,[x9,x10,lsl #3]
	cbnz	x28,2f
1:	adrp	x28,__ape_boot_tls_end
	add	x28,x28,#:lo12:__ape_boot_tls_end
2:	ret
	.endfn	__ape_load_tls,globl,hidden

//	What dyld calls, since the startup it calls is C and would be
//	reading a thread pointer that isn't there yet.
	.text
cosmo_dylib_routine:
	stp	x29,x30,[sp,#-32]!
	mov	x29,sp
	str	x28,[sp,#16]
	bl	__ape_load_tls
#ifndef APE_DYLIB_NO_AUTOBOOT
	bl	cosmo_dylib_routine_impl
#endif
	ldr	x28,[sp,#16]
	ldp	x29,x30,[sp],#32
	ret
	.endfn	cosmo_dylib_routine,globl

#endif /* __aarch64__ */
