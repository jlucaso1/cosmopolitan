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
#define LIBRARY "./cosmo_dll_test.dylib"
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

int main(void) {
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
  int booted = init();
  {
    // a bare win32 call, before anything that might touch the registers
    // the microsoft convention has the callee preserve
    DWORD wrote;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "back from the guest\n", 20,
              &wrote, 0);
  }
  printf("ok: the runtime booted, returning %d\n", booted);
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
