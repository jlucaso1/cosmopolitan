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
#include "libc/intrin/weaken.h"
#include "libc/runtime/internal.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/stdio.h"
#include "libc/sysv/pib.h"
#include "libc/thread/tls.h"

/**
 * @fileoverview Bringing up the runtime inside a mach-o host process.
 *
 * The windows half of this is libc/runtime/windllboot.c and the elf half
 * is test/ape/dso/dso.c. What is particular to this one is the segment
 * register: on xnu cosmopolitan keeps its thread information block at a
 * fixed offset from %gs, and so does the host's libpthread, so a library
 * can't have it to itself.
 *
 * The way out is the same as on linux, only the slot comes from
 * elsewhere. pthread_key_create() hands the host a key, and on x86-64
 * the storage for key k is at %gs:(k*8), which is a displacement of
 * exactly the kind __tls_disp holds. So the host creates a key and says
 * where it landed.
 */

extern char cosmo_dylib_hostos asm("__hostos");

extern long __tls_disp;
extern char __tls_guest;
extern bool __cosmo_hosted;

void _init(void);

// the plain __get_tls() is a bare %fs read, which is what tlscc exists to
// rewrite; this asks for the getter by name, since the file is compiled
// without it and xnu keeps the tib somewhere else entirely
struct CosmoTib *__get_tls_rax(void);

// weak, like cosmo2.c declares them: they come from the linker script,
// so no package defines them
typedef int init_f(int, char **, char **, unsigned long *);
extern init_f *__init_array_start[] __attribute__((__weak__));
extern init_f *__init_array_end[] __attribute__((__weak__));

static void trace(const char *tag, uintptr_t v) {
  char buf[28] = "boot ....  0000000000000000\n";
  for (int i = 0; i < 4 && tag[i]; ++i)
    buf[5 + i] = tag[i];
  for (int i = 0; i < 16; ++i)
    buf[25 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15];
  long ax;
  asm volatile("syscall"
               : "=a"(ax)
               : "0"(0x2000004), "D"(2l), "S"(buf), "d"(28l)
               : "rcx", "r11", "memory", "cc");
}

static unsigned long empty_auxv[2];
static bool cosmo_dylib_booted;
static char *cosmo_dylib_argv[2];
static char *cosmo_dylib_environ[1];
struct CosmoTib *__cosmo_dylib_main_tib;

/**
 * Starts Cosmopolitan Libc inside a host process.
 *
 * The host passes what the kernel would have passed a program, plus the
 * displacement of the thread local slot it set aside for the tib, which
 * on this platform is a pthread key times eight. Zero leaves
 * cosmopolitan reading its own segment base, which only works if nothing
 * else in the process is using it.
 *
 * Safe to call more than once; only the first call does anything.
 * Returns nonzero if the runtime is up. An int rather than a bool
 * because the host is built by another compiler.
 */
int cosmo_dylib_boot(int argc, char **argv, char **envp, long tls_disp) {
  if (cosmo_dylib_booted)
    return 1;
  cosmo_dylib_booted = true;

  trace("in", tls_disp);
  trace("&os", (uintptr_t)&cosmo_dylib_hostos);
  trace("&ho", (uintptr_t)&__cosmo_hosted);
  trace("&te", (uintptr_t)&__tls_enabled);
  trace("&td", (uintptr_t)&__tls_disp);
  trace("self", (uintptr_t)&cosmo_dylib_booted);
  cosmo_dylib_hostos = _HOSTXNU;
  trace("w1", 0);
  __cosmo_hosted = true;
  trace("w2", 0);
  __tls_enabled = false;
  trace("w3", 0);

  if (tls_disp) {
    __tls_disp = tls_disp;
    __tls_guest = 1;
  }

  trace("flag", 0);
  __pagesize = 4096;
  __gransize = 4096;

  // The host owns the process, so take its identity rather than minting
  // one the way a program's startup does. Raw, because the wrappers
  // dispatch through numbers that one of the fragments below decodes,
  // and __maps_init() wants a pid before then.
  long pid;
  asm volatile("syscall"
               : "=a"(pid)
               : "0"(0x2000014)  // xnu getpid
               : "rcx", "r11", "memory", "cc");
  trace("pid", (uintptr_t)pid);
  __get_pib()->pid = pid;
  trace("pib", (uintptr_t)__get_pib());

  if (!envp)
    envp = cosmo_dylib_environ;
  if (!environ)
    environ = envp;

  // libc/crt/crt.S does these before entering the runtime, and _init
  // doesn't repeat them; __get_main_stack() reads __envp directly
  trace("env", (uintptr_t)envp);
  __oldstack = (intptr_t)__builtin_frame_address(0);

  // Past the environment's terminator is where a program finds its
  // auxiliary vector, on the systems that have one. This one doesn't:
  // xnu puts its own array of strings there, and walking that as pairs
  // of numbers goes wherever it goes. Nothing in the runtime needs it
  // here, since the page size is already set above.
  unsigned long *auxv = empty_auxv;

  if (!argv) {
    cosmo_dylib_argv[0] = (char *)"cosmo";
    argv = cosmo_dylib_argv;
    argc = 1;
  }

  // decentralized init: system call dispatch, memory map, the lot. it
  // wants argc/argv/envp/auxv in r12 through r15, and a couple of the
  // fragments dereference argv, so give it something valid.
  trace("pre", (uintptr_t)auxv);
  register long r12 asm("r12") = argc;
  register char **r13 asm("r13") = argv;
  register char **r14 asm("r14") = envp;
  register unsigned long *r15 asm("r15") = auxv;
  asm volatile("call\t_init"
               : "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
               : /* no inputs */
               : "rdi", "rsi", "rbx", "rax", "rcx", "rdx", "r8", "r9", "r10",
                 "r11", "memory", "cc");

  // cosmo.S copies these out of the registers _init takes, and it isn't
  // linked into a library, so a constructor that reads them would find an
  // argument count with nothing under it
  trace("init", 0);
  __argc = argc;
  __argv = argv;
  __envp = envp;

  // these two live in cosmo.S as well, so the linker never pulls them in
  // and their .init fragments never run
  trace("tls", 0);
  __enable_tls();

  // the constructors, which nothing else is going to run: dyld only runs
  // what LC_ROUTINES or __mod_init_func point at, and this emits
  // neither, on purpose. malloc's dispatch is one of them, so they go
  // before anything that allocates.
  for (init_f **f = __init_array_start; f < __init_array_end; ++f) {
    trace("ctor", (uintptr_t)*f);
    (*f)(argc, argv, envp, auxv);
  }

  trace("fds", 0);
  if (_weaken(__init_fds))
    _weaken(__init_fds)();

  trace("done", 0);
  __cosmo_dylib_main_tib = __get_tls_rax();
  return 1;
}

/**
 * Shuts the runtime down without shutting the process down.
 *
 * exit() ends the process, which for a hosted module means taking the
 * host with it, and the atexit handlers hang off that path so they never
 * run when the host exits instead. The buffered streams are ours alone,
 * and nobody else will flush them.
 */
void cosmo_dylib_fini(void) {
  if (_weaken(fflush))
    _weaken(fflush)(0);
}
