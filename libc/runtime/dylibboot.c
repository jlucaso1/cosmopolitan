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

#ifdef __x86_64__
extern long __tls_disp;
extern char __tls_guest;
#endif
extern long __cosmo_hosted_tls_key;
extern bool __cosmo_hosted;

void _init(void);

// the same thing, said the way the other architecture calls it
extern void _init_with_args(int, char **, char **,
                            unsigned long *) asm("_init");

// the plain __get_tls() is a bare %fs read, which is what tlscc exists to
// rewrite; this asks for the getter by name, since the file is compiled
// without it and xnu keeps the tib somewhere else entirely
#ifdef __x86_64__
struct CosmoTib *__get_tls_rax(void);
#define __get_tls_here() __get_tls_rax()
#else
#define __get_tls_here() __get_tls()
#endif

// weak, like cosmo2.c declares them: they come from the linker script,
// so no package defines them
typedef int init_f(int, char **, char **, unsigned long *);
extern init_f *__init_array_start[] __attribute__((__weak__));
extern init_f *__init_array_end[] __attribute__((__weak__));

/**
 * Asks the kernel what process this is, without the wrappers.
 *
 * They dispatch through system call numbers that one of the init
 * fragments decodes, and the memory manager wants a pid before then.
 */
static long xnu_getpid(void) {
  long pid;
#ifdef __x86_64__
  asm volatile("syscall"
               : "=a"(pid)
               : "0"(0x2000014)
               : "rcx", "r11", "memory", "cc");
#else
  register long x16 asm("x16") = 20;
  register long x0 asm("x0");
  asm volatile("svc\t#0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
  pid = x0;
#endif
  return pid;
}

/**
 * Runs the decentralized startup.
 *
 * On one architecture it wants its arguments in the callee saved
 * registers it walks its data with; on the other it is an ordinary
 * function.
 */
static void run_init(int argc, char **argv, char **envp, unsigned long *auxv) {
#ifdef __x86_64__
  register long r12 asm("r12") = argc;
  register char **r13 asm("r13") = argv;
  register char **r14 asm("r14") = envp;
  register unsigned long *r15 asm("r15") = auxv;
  asm volatile("call\t_init"
               : "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
               : /* no inputs */
               : "rdi", "rsi", "rbx", "rax", "rcx", "rdx", "r8", "r9", "r10",
                 "r11", "memory", "cc");
#else
  _init_with_args(argc, argv, envp, auxv);
#endif
}

static void trace(const char *tag, uintptr_t v) {
  char buf[28] = "boot ....  0000000000000000\n";
  for (int i = 0; i < 4 && tag[i]; ++i)
    buf[5 + i] = tag[i];
  for (int i = 0; i < 16; ++i)
    buf[25 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15];
#ifdef __x86_64__
  long ax;
  asm volatile("syscall"
               : "=a"(ax)
               : "0"(0x2000004), "D"(2l), "S"(buf), "d"(28l)
               : "rcx", "r11", "memory", "cc");
#else
  register long x16 asm("x16") = 4;
  register long x0 asm("x0") = 2;
  register char *x1 asm("x1") = buf;
  register long x2 asm("x2") = 28;
  asm volatile("svc\t#0x80"
               : "+r"(x0)
               : "r"(x16), "r"(x1), "r"(x2)
               : "memory", "cc");
#endif
}

static unsigned long empty_auxv[2];
static bool cosmo_dylib_booted;
static char *cosmo_dylib_argv[2];
static char *cosmo_dylib_environ[1];
extern struct CosmoTib *__cosmo_hosted_main_tib;

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
  cosmo_dylib_hostos = _HOSTXNU;
  __cosmo_hosted = true;
  __tls_enabled_set(false);

  if (tls_disp) {
#ifdef __x86_64__
    __tls_disp = tls_disp;
    __tls_guest = 1;
#else
    __cosmo_hosted_tls_key = tls_disp / sizeof(void *);
#endif
  }

  // apple silicon has bigger pages than intel does
#ifdef __x86_64__
  __pagesize = 4096;
#else
  __pagesize = 16384;
#endif
  __gransize = __pagesize;

  // The host owns the process, so take its identity rather than minting
  // one the way a program's startup does. Raw, because the wrappers
  // dispatch through numbers that one of the fragments below decodes,
  // and __maps_init() wants a pid before then.
  trace("flag", 0);
  __get_pib()->pid = xnu_getpid();
  trace("pid", 0);

  if (!envp)
    envp = cosmo_dylib_environ;
  if (!environ)
    environ = envp;

  // libc/crt/crt.S does these before entering the runtime, and _init
  // doesn't repeat them; __get_main_stack() reads __envp directly
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
  trace("pre", 0);
  run_init(argc, argv, envp, auxv);
  trace("init", 0);

  // cosmo.S copies these out of the registers _init takes, and it isn't
  // linked into a library, so a constructor that reads them would find an
  // argument count with nothing under it
  __argc = argc;
  __argv = argv;
  __envp = envp;

  // these two live in cosmo.S as well, so the linker never pulls them in
  // and their .init fragments never run
  trace("tls", 0);
  __enable_tls();
  trace("tls2", 0);

  // the constructors, which nothing else is going to run: dyld only runs
  // what LC_ROUTINES or __mod_init_func point at, and this emits
  // neither, on purpose. malloc's dispatch is one of them, so they go
  // before anything that allocates.
  for (init_f **f = __init_array_start; f < __init_array_end; ++f) {
    (*f)(argc, argv, envp, auxv);
  }

  trace("fds", 0);
  if (_weaken(__init_fds))
    _weaken(__init_fds)();

  trace("done", 0);
  __cosmo_hosted_main_tib = __get_tls_here();
  return 1;
}

/**
 * Where dyld puts the host's pthread_key_create().
 *
 * Bound before anything in the library runs; see the bind opcodes in
 * __LINKEDIT. This is the one thing a guest can't do for itself: a
 * thread local slot has to come from whoever is already handing them
 * out, or two runtimes end up writing to the same place.
 */
int (*__ape_pthread_key_create)(unsigned *, void (*)(void *));

/**
 * Brings the runtime up as soon as the library is loaded.
 *
 * What dyld calls for LC_ROUTINES, with what it would pass an
 * initializer. A host that only knows how to open a library gets one
 * that works; a host with opinions about the thread local slot can call
 * cosmo_dylib_boot() itself first, and this finds nothing left to do.
 */
#ifdef __x86_64__
#define COSMO_DYLIB_ROUTINE cosmo_dylib_routine
#else
// the thunk in ape/dylibthunk.S wears the name, since this can't run
// until something has put a thread pointer in the register it reads
#define COSMO_DYLIB_ROUTINE cosmo_dylib_routine_impl
#endif

void COSMO_DYLIB_ROUTINE(int argc, char **argv, char **envp, char **apple,
                         void *vars) {
  trace("rout", (uintptr_t)__ape_pthread_key_create);
  long disp = 0;
  unsigned key;
  if (__ape_pthread_key_create && !__ape_pthread_key_create(&key, 0))
    disp = (long)key * sizeof(void *);
  cosmo_dylib_boot(argc, argv, envp, disp);
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
