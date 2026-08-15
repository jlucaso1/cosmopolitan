// The host half of the hosted shared object test.
//
// Deliberately not cosmopolitan: built by the platform's own toolchain
// against its own libc, so what it proves is that a stock ELF process can
// carry cosmopolitan inside it, rather than that cosmopolitan agrees with
// itself.
//
// The thread local slot below is the whole of the arrangement between the
// two. Cosmopolitan keeps its thread information block at a fixed offset
// from %fs, and so does glibc, so a hosted module can't have the segment
// register to itself. Instead the host reserves a slot and passes its
// displacement, and cosmopolitan reads through that from then on.

#define _GNU_SOURCE
#include <asm/prctl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LIBRARY "./cosmo_dso_test.so"

extern char **environ;

// initial-exec, so its displacement from the segment base is fixed and
// the getters in libc/sysv/tlsasm.S can reach it without a function call
__thread void *cosmo_tib __attribute__((tls_model("initial-exec")));

static void (*guest_init)(int, char **, char **, long);
static void (*guest_fini)(void);
static int (*guest_thread_init)(void);
static void (*guest_thread_fini)(void);
static int (*guest_add)(int, int);
static int (*guest_probe)(char *, int);

static unsigned long fsbase(void) {
  unsigned long base = 0;
  syscall(SYS_arch_prctl, ARCH_GET_FS, &base);
  return base;
}

static int failed;

static void check(int ok, const char *what) {
  printf("%s: %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    failed = 1;
}

// a thread the host made, which enters the guest with an empty tib slot
static void *thread_main(void *arg) {
  char buf[128] = "";
  if (guest_thread_init() != 0) {
    check(0, "adopting a host thread");
    return 0;
  }
  int pid = guest_probe(buf, sizeof(buf));
  check(pid == getpid(), "guest getpid on a host thread");
  // the guest has to know it isn't the thread it started on
  char mine[32];
  snprintf(mine, sizeof(mine), "tid=%d", (int)syscall(SYS_gettid));
  check(strstr(buf, mine) != NULL, "guest gettid on a host thread");
  printf("      %s\n", buf);
  guest_thread_fini();
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : LIBRARY;

  unsigned long before = fsbase();
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("FAIL: dlopen: %s\n", dlerror());
    return 1;
  }
  check(fsbase() == before, "host thread pointer survives dlopen");

#define GET(v, n)                                  \
  do {                                             \
    *(void **)&v = dlsym(h, n);                    \
    if (!v) {                                      \
      printf("FAIL: dlsym %s: %s\n", n, dlerror()); \
      return 2;                                    \
    }                                              \
  } while (0)
  GET(guest_init, "cosmo_dso_init");
  GET(guest_fini, "cosmo_dso_fini");
  GET(guest_thread_init, "cosmo_dso_thread_init");
  GET(guest_thread_fini, "cosmo_dso_thread_fini");
  GET(guest_add, "cosmo_dso_add");
  GET(guest_probe, "cosmo_dso_probe");
#undef GET

  long disp = (char *)&cosmo_tib - (char *)before;
  printf("      hosted tls slot at %%fs%+ld\n", disp);
  // A host that has arguments can pass them, and one that doesn't can
  // say nothing at all: an addon is opened by a runtime that never
  // offered any. Both are worth covering, and the second is the one
  // that finds things.
  if (getenv("COSMO_DSO_BOOT_BARE")) {
    void (*bare)(void) = (void (*)(void))sym(h, "cosmo_dso_boot");
    if (!bare) {
      printf("FAIL: no cosmo_dso_boot\n");
      return 3;
    }
    printf("      starting the guest with nothing to go on\n");
    bare();
  } else {
    guest_init(argc, argv, environ, disp);
  }
  check(fsbase() == before, "host thread pointer survives guest startup");

  check(guest_add(20, 22) == 42, "cosmo_dso_add(20, 22) = 42");

  char buf[128] = "";
  int pid = guest_probe(buf, sizeof(buf));
  check(pid == getpid(), "guest getpid agrees with the host");
  check(strstr(buf, "cosmo libc says") != NULL, "guest snprintf and malloc");
  printf("      %s\n", buf);

  pthread_t t;
  pthread_create(&t, 0, thread_main, 0);
  pthread_join(t, 0);

  check(fsbase() == before, "host thread pointer intact at the end");
  guest_fini();

  printf("%s\n", failed ? "the guest is not well" : "all good");
  return failed;
}
