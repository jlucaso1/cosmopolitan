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

//	Says how far _init got, for when it doesn't get all the way.
//
//	_init is a chain of fragments the linker sorts by priority and runs
//	by falling through one into the next, so a marker is just another
//	fragment. Everything is saved: %rdi and %rsi walk the decentralized
//	data in lockstep with the code, and %r12 through %r15 carry what
//	the startup was handed.

	.macro	.initmark number:req name:req text:req
	.section .rodata.str1.1,"aMS",@progbits,1
1:	.ascii	"\text\n"
2:
	.previous
	.init.start \number,_init_\name
	push	%rax
	push	%rcx
	push	%rdx
	push	%rsi
	push	%rdi
	push	%r8
	push	%r9
	push	%r10
	push	%r11
	lea	1b(%rip),%rsi
	mov	$0x2000004,%eax			// xnu write
	mov	$2,%edi
	mov	$(2b-1b),%edx
	syscall
	pop	%r11
	pop	%r10
	pop	%r9
	pop	%r8
	pop	%rdi
	pop	%rsi
	pop	%rdx
	pop	%rcx
	pop	%rax
	.init.end \number,_init_\name
	.endm

	.initmark 100,aa_mark_100,"init 100"
	.initmark 200,aa_mark_200,"init 200"
	.initmark 300,aa_mark_300,"init 300 begins"
	.initmark 300,p0mark,"init 300 argv and environ done"
	.initmark 300,q_mark,"init 300 progname done"
	.initmark 300,t_mark,"init 300 systemfive done"
	.initmark 301,aa_mark_301,"init 301 begins"
	.initmark 301,z_mark_301,"init 301 maps and nocolor done"
	.initmark 302,aa_mark_302,"init 302"
	.initmark 305,aa_mark_305,"init 305"
	.initmark 400,aa_mark_400,"init 400"
	.initmark 500,aa_mark_500,"init 500"
	.initmark 700,aa_mark_700,"init 700"
