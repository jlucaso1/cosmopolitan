// The host half of the shared object test.
//
// Deliberately not cosmopolitan: this is built by the platform's own
// toolchain, so what it proves is that a native loader accepts what ape
// emitted, rather than that cosmopolitan agrees with itself.

#include <stdio.h>

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

int main(void) {
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
  return 0;
}
