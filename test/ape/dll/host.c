// The host half of the shared object test.
//
// Deliberately not cosmopolitan: this is built by the platform's own
// toolchain, so what it proves is that a native loader accepts what ape
// emitted, rather than that cosmopolitan agrees with itself.

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define LIBRARY "cosmo_dll_test.dll"
static void *load(const char *path) {
  return (void *)LoadLibraryA(path);
}
static void *sym(void *h, const char *name) {
  return (void *)GetProcAddress((HMODULE)h, name);
}
static const char *why(void) {
  static char buf[64];
  snprintf(buf, sizeof(buf), "error %lu", GetLastError());
  return buf;
}
#else
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#define LIBRARY "./cosmo_dll_test.dylib"
extern char **environ;
static void *load(const char *path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *sym(void *h, const char *name) {
  return dlsym(h, name);
}
static const char *why(void) {
  const char *e = dlerror();
  return e ? e : "(no error reported)";
}
#endif

#ifdef _WIN32
static int (*guest_thread_init)(void);
static void (*guest_thread_fini)(void);
static int (*guest_probe)(char *, int);
static int thread_failed;
static char main_tid[32];

// a thread the host made, which enters the guest with an empty tib slot
static DWORD WINAPI thread_main(LPVOID arg) {
  char buf[128] = "";
  if (guest_thread_init() != 0) {
    printf("FAIL: could not adopt a host thread\n");
    thread_failed = 1;
    return 0;
  }
  int pid = guest_probe(buf, sizeof(buf));
  if (pid != (int)GetCurrentProcessId()) {
    printf("FAIL: guest getpid said %d on a host thread\n", pid);
    thread_failed = 1;
  }
  char mine[32];
  snprintf(mine, sizeof(mine), "tid=%d", (int)GetCurrentThreadId());
  if (!strstr(buf, mine)) {
    printf("FAIL: guest said \"%s\", this thread is %s\n", buf, mine);
    thread_failed = 1;
  }
  guest_thread_fini();
  if (!thread_failed)
    printf("ok: a host thread reached the guest: %s\n", buf);
  return 0;
}
#endif

#ifdef __APPLE__
// Where the storage for a pthread key lives, relative to the segment
// base. This is the arrangement the whole port rests on: cosmopolitan
// keeps its thread information block at a fixed offset from %gs, and so
// does libpthread, so the host hands over a slot of its own instead.
static long tls_displacement(pthread_key_t key) {
  return (long)key * sizeof(void *);
}

static int (*guest_thread_init)(void);
static void (*guest_thread_fini)(void);
static int (*guest_probe_fn)(char *, int);
static int thread_failed;
static char main_tid[32];

// a thread the host made, which enters the guest with an empty tib slot
static void *thread_main(void *arg) {
  char buf[128] = "";
  if (guest_thread_init() != 0) {
    printf("FAIL: could not adopt a host thread\n");
    thread_failed = 1;
    return 0;
  }
  int pid = guest_probe_fn(buf, sizeof(buf));
  if (pid != (int)getpid()) {
    printf("FAIL: guest getpid said %d on a host thread\n", pid);
    thread_failed = 1;
  }
  // the guest has to know it isn't the thread it started on. Its own
  // numbering, not the host's: gettid() on this platform answers with a
  // mach port, which is nothing like a pthread's id.
  if (strstr(buf, main_tid)) {
    printf("FAIL: guest reports %s on a thread that isn't it\n", main_tid);
    thread_failed = 1;
  }
  guest_thread_fini();
  if (!thread_failed)
    printf("ok: a host thread reached the guest: %s\n", buf);
  return 0;
}

static void *read_gs(long disp) {
  void *value;
  asm("mov\t%%gs:(%1),%0" : "=r"(value) : "r"(disp));
  return value;
}
#endif

int main(int argc, char **argv) {
  // unbuffered, so a crash in the loader still leaves a trail
  setvbuf(stdout, 0, _IONBF, 0);
  printf("loading %s\n", LIBRARY);
  void *h = load(LIBRARY);
  if (!h) {
    printf("FAIL: could not load %s: %s\n", LIBRARY, why());
    return 1;
  }
  printf("ok: loaded %s at %p\n", LIBRARY, h);

  int (*add)(int, int) = (int (*)(int, int))sym(h, "cosmo_dll_add");
  if (!add) {
    printf("FAIL: could not find cosmo_dll_add: %s\n", why());
    return 2;
  }
  printf("ok: resolved cosmo_dll_add at %p\n", (void *)add);

  int got = add(20, 22);
  if (got != 42) {
    printf("FAIL: cosmo_dll_add(20, 22) returned %d, wanted 42\n", got);
    return 3;
  }
  printf("ok: cosmo_dll_add(20, 22) = %d\n", got);

#ifdef __APPLE__
  // check the arrangement before relying on it: what pthread_setspecific
  // writes has to be what a read through the displacement finds
  pthread_key_t key;
  if (pthread_key_create(&key, 0)) {
    printf("FAIL: could not create a thread local slot\n");
    return 4;
  }
  long disp = tls_displacement(key);
  pthread_setspecific(key, (void *)0x1234);
  if (read_gs(disp) != (void *)0x1234) {
    printf("FAIL: %%gs%+ld is not where key %d lives\n", disp, (int)key);
    return 5;
  }
  pthread_setspecific(key, 0);
  printf("ok: the guest can keep its tib at %%gs%+ld\n", disp);

  // Nothing here starts the runtime. dyld ran it when the library was
  // loaded, before this program got control back, so what follows is
  // asking a library that was only opened to do work.
  int (*probe)(char *, int) = (int (*)(char *, int))sym(h, "cosmo_dll_probe");
  void (*fini)(void) = (void (*)(void))sym(h, "cosmo_dylib_fini");
  if (!probe || !fini) {
    printf("FAIL: could not find the runtime entry points: %s\n", why());
    return 6;
  }

  char buf[128] = "";
  int pid = probe(buf, sizeof(buf));
  if (pid != (int)getpid()) {
    printf("FAIL: guest getpid said %d, this process is %d\n", pid,
           (int)getpid());
    return 8;
  }
  if (!strstr(buf, "cosmo libc says")) {
    printf("FAIL: guest snprintf produced \"%s\"\n", buf);
    return 9;
  }
  printf("ok: the guest runtime is up: %s\n", buf);
  {
    const char *at = strstr(buf, "tid=");
    if (!at) {
      printf("FAIL: the guest didn't say which thread it was on\n");
      return 10;
    }
    snprintf(main_tid, sizeof(main_tid), "%s", at);
  }

  // node calls a native addon from whatever thread it likes, so a hosted
  // runtime that only works on the one that loaded it is of little use
  guest_probe_fn = probe;
  guest_thread_init = (int (*)(void))sym(h, "cosmo_dylib_thread_init");
  guest_thread_fini = (void (*)(void))sym(h, "cosmo_dylib_thread_fini");
  if (!guest_thread_init || !guest_thread_fini) {
    printf("FAIL: could not find the thread entry points: %s\n", why());
    return 10;
  }
  pthread_t t;
  pthread_create(&t, 0, thread_main, 0);
  pthread_join(t, 0);
  if (thread_failed)
    return 11;

  fini();
#endif

#ifdef _WIN32
  // The library carries the whole libc on this platform, so it can be
  // asked something only a running runtime can answer. The pid is a
  // guest system call whose right answer this process already knows,
  // and the text around it came out of the guest's own snprintf and
  // heap.
  int (*init)(void) = (int (*)(void))sym(h, "cosmo_dll_init");
  if (!init) {
    printf("FAIL: could not find cosmo_dll_init: %s\n", why());
    return 4;
  }
  printf("ok: the runtime booted, returning %d\n", init());
  printf("ok: the library still answers: %d\n", add(1, 2));

  int (*probe)(char *, int) = (int (*)(char *, int))sym(h, "cosmo_dll_probe");
  if (!probe) {
    printf("FAIL: could not find cosmo_dll_probe: %s\n", why());
    return 4;
  }
  char buf[128] = "";
  int pid = probe(buf, sizeof(buf));
  if (pid != (int)GetCurrentProcessId()) {
    printf("FAIL: guest getpid said %d, this process is %d\n", pid,
           (int)GetCurrentProcessId());
    return 5;
  }
  if (!strstr(buf, "cosmo libc says")) {
    printf("FAIL: guest snprintf produced \"%s\"\n", buf);
    return 6;
  }
  printf("ok: the guest runtime is up: %s\n", buf);
  {
    const char *at = strstr(buf, "tid=");
    if (!at) {
      printf("FAIL: the guest didn't say which thread it was on\n");
      return 10;
    }
    snprintf(main_tid, sizeof(main_tid), "%s", at);
  }

  // node calls a native addon from its own threads, so a hosted runtime
  // that only works on the one that loaded it is of little use
  guest_probe = probe;
  guest_thread_init = (int (*)(void))sym(h, "cosmo_dll_thread_init");
  guest_thread_fini = (void (*)(void))sym(h, "cosmo_dll_thread_fini");
  if (!guest_thread_init || !guest_thread_fini) {
    printf("FAIL: could not find the thread entry points: %s\n", why());
    return 7;
  }
  HANDLE t = CreateThread(0, 0, thread_main, 0, 0, 0);
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
  if (thread_failed)
    return 8;
#endif

  return 0;
}
