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
#include "libc/calls/calls.h"
#include "libc/calls/syscall-sysv.internal.h"
#include "libc/intrin/atomic.h"
#include "libc/intrin/maps.h"
#include "libc/mem/mem.h"
#include "libc/runtime/internal.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/o.h"
#include "libc/sysv/consts/prot.h"
#include "libc/thread/tls.h"
#include "third_party/dlmalloc/dlmalloc.h"

/**
 * @fileoverview Bringing up the runtime inside somebody else's process.
 *
 * The windows half of this is libc/runtime/windllboot.c. Here the host is
 * an ELF program with its own libc, and the two have to share a machine
 * neither of them owns: the segment register belongs to the host, the
 * process belongs to the host, and exiting is the host's decision.
 *
 * Startup is explicit rather than automatic. The loader would run us at
 * dlopen time, which is too early for anything that makes a system call,
 * so the host calls cosmo_dso_init() when it's ready.
 */

extern void _init(void);

// hosted tls: where the tib sits relative to the host's segment base
extern long __tls_disp;
extern char __tls_guest;

typedef int init_f(int, char **, char **, unsigned long *);
extern init_f *__init_array_start[];
extern init_f *__init_array_end[];

static unsigned long empty_auxv[2];
static struct CosmoTib *main_tib;

/**
 * Stands in for DT_INIT.
 *
 * _init is the symbol GNU ld picks by default, and it is not a function
 * here but a chain of fragments expecting argc through auxv in r12-r15.
 * Called by the loader with whatever happens to be in those, _init_pagesize
 * dereferences r15 as an auxiliary vector.
 */
void cosmo_dso_noop(void) {
}

void cosmo_dso_init(int, char **, char **, long);

// initial-exec, so its displacement from the segment base is fixed and
// the same for every thread, which is what makes it usable as the slot
static __thread void *cosmo_dso_tib __attribute__((tls_model("initial-exec")));

/**
 * Where this thread's storage begins, without reading it.
 *
 * The obvious way is a bare %fs read, which is exactly what the wrapper
 * this file is compiled with rewrites into a call to the getters, and
 * the getters are what we're on our way to setting up.
 */
static long cosmo_dso_segment_base(void) {
  long base = 0;
  register long rax asm("rax") = 158;      // arch_prctl
  register long rdi asm("rdi") = 0x1003;   // ARCH_GET_FS
  register long *rsi asm("rsi") = &base;
  asm volatile("syscall"
               : "+r"(rax)
               : "r"(rdi), "r"(rsi)
               : "rcx", "r11", "memory", "cc");
  return base;
}

/**
 * Brings the runtime up when the library is loaded.
 *
 * What the link points DT_INIT at, so that a host which only knows how
 * to open a library gets one that works. The windows half of this is the
 * library entry point and the mach-o half is LC_ROUTINES.
 *
 * The thread local slot comes from the library itself here, which the
 * other two can't do: an initial-exec variable is laid down by the same
 * loader that placed us, at a displacement every thread shares.
 */
void cosmo_dso_autoboot(void) {
  cosmo_dso_init(0, 0, environ,
                 (char *)&cosmo_dso_tib - (char *)cosmo_dso_segment_base());
}

/**
 * Starts Cosmopolitan Libc inside a host process.
 *
 * The host passes what the kernel would have passed a program, plus the
 * displacement of a thread local slot it set aside for the tib. Zero
 * leaves cosmopolitan reading its own segment base, which only works if
 * nothing else in the process is using it.
 */
void cosmo_dso_init(int argc, char **argv, char **envp, long tls_disp) {
  // the library brings itself up when it's opened, so a host that also
  // asks finds it already done rather than doing it twice
  static bool booted;
  if (booted)
    return;
  booted = true;

  if (tls_disp) {
    __tls_disp = tls_disp;
    __tls_guest = 1;
  }

  unsigned long *auxv;
  if (envp) {
    // it sits past the environment's terminator, which is how every
    // runtime finds it without being told
    char **p = envp;
    while (*p)
      ++p;
    auxv = (unsigned long *)(p + 1);
  } else {
    auxv = empty_auxv;
  }

  // libc/crt/crt.S does these before entering the runtime, and _init
  // doesn't repeat them; __get_main_stack() reads __envp directly
  __envp = envp;
  __oldstack = (intptr_t)__builtin_frame_address(0);

  register long r12 asm("r12") = argc;
  register char **r13 asm("r13") = argv;
  register char **r14 asm("r14") = envp;
  register unsigned long *r15 asm("r15") = auxv;
  asm volatile("call\t_init"
               : "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
               : /* no inputs */
               : "rdi", "rsi", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11",
                 "memory", "cc");

  // these two live in cosmo.S, which nothing references in a shared
  // object, so the linker never pulls it in and its .init fragments
  // never run
  __enable_tls();
  __init_fds();
  main_tib = __get_tls();

  // and now the constructors the linker script held back
  for (init_f **f = __init_array_start; f != __init_array_end; ++f)
    (*f)(argc, argv, envp, auxv);
}

/**
 * Exact bounds of the mapping an address falls in.
 *
 * The scratch buffer has to be off the stack. This runs to find the very
 * mapping that would let read() accept a stack buffer, so using one would
 * be the same chicken and egg. The heap is already accounted for.
 */
static bool find_mapping(uintptr_t addr, uintptr_t *out_lo, uintptr_t *out_hi) {
#define MAPS_BUF 8192
  char *buf = malloc(MAPS_BUF);
  if (!buf)
    return false;
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) {
    free(buf);
    return false;
  }
  bool found = false;
  ssize_t n, carry = 0;
  while (!found && (n = read(fd, buf + carry, MAPS_BUF - carry - 1)) > 0) {
    n += carry;
    buf[n] = 0;
    char *line = buf, *nl;
    while ((nl = strchr(line, '\n'))) {
      *nl = 0;
      uintptr_t lo = 0, hi = 0;
      char *p = line;
      for (; *p && *p != '-'; ++p)
        lo = lo * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
      if (*p == '-')
        for (++p; *p && *p != ' '; ++p)
          hi = hi * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
      if (lo <= addr && addr < hi) {
        *out_lo = lo, *out_hi = hi, found = true;
        break;
      }
      line = nl + 1;
    }
    carry = buf + n - line;
    if (carry >= MAPS_BUF - 1)
      carry = 0;  // a line this long isn't one of ours
    else
      memmove(buf, line, carry);
  }
  close(fd);
  free(buf);
  return found;
}

/**
 * Adopts a thread the host created.
 *
 * Only the thread that called cosmo_dso_init() has a tib. Any other one
 * arrives with an empty slot, and the first thing to want thread local
 * storage, which is to say malloc or errno or stdio, reads a null
 * pointer. Cosmopolitan does this itself for threads it creates, where
 * pthread_create() builds the tib and clone() installs it.
 */
int cosmo_dso_thread_init(void) {
  if (!main_tib)
    return -1;
  if (__get_tls())
    return 0;  // already adopted

  // _mktls() copies ftrace, strace and the signal mask off the current
  // tib, so it needs a valid one installed; lend it the main thread's
  __set_tls(main_tib);
  struct CosmoTib *tib;
  char *tls = _mktls(&tib);
  if (!tls) {
    __set_tls(0);
    return -1;
  }
  tib->tib_malloc = tmspace_acquire();

  // __maps_init() only knows about the main stack, and kisdangerous()
  // rejects what it hasn't seen, so a read() into a buffer on this
  // thread's stack would come back EFAULT
  uintptr_t lo, hi;
  if (find_mapping((uintptr_t)__builtin_frame_address(0), &lo, &hi))
    __maps_track((char *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_NOFORK);

  tib->tib_keys_dynamic = (void *)tls;  // so the teardown can free it

  // not gettid(), which answers out of the tib, and the tib installed
  // just now is still the main thread's
  int tid = sys_gettid();
  atomic_init(&tib->tib_ptid, tid);
  atomic_init(&tib->tib_ctid, tid);
  __set_tls(tib);
  return 0;
}

/**
 * Hands back what cosmo_dso_thread_init() took.
 */
void cosmo_dso_thread_fini(void) {
  struct CosmoTib *tib = __get_tls();
  if (!tib || tib == main_tib)
    return;
  void *tls = tib->tib_keys_dynamic;
  tmspace_release(tib->tib_malloc);
  __set_tls(0);
  free(tls);
}

/**
 * Shuts the runtime down without shutting the process down.
 *
 * exit() ends the process, which for a hosted module means taking the
 * host with it, and the atexit handlers hang off that path so they never
 * run when the host exits instead. The buffered streams are ours alone,
 * and nobody else will flush them.
 */
void cosmo_dso_fini(void) {
  fflush(0);
}
