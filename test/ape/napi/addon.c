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
#include "libc/calls/calls.h"
#include "libc/dce.h"
#include "libc/fmt/conv.h"
#include "libc/mem/mem.h"
#include "libc/nt/dll.h"
#include "libc/nt/runtime.h"
#include "libc/nt/thunk/msabi.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"

/**
 * @fileoverview A node addon built out of the cosmopolitan libc.
 *
 * The thing the whole shared library effort was for. Node opens this the
 * way it opens any native addon and calls napi_register_module_v1, and
 * what answers is a runtime of our own, running inside node's process,
 * with its own heap and its own system calls.
 *
 * N-API is a stable C ABI, so the handful of entry points needed are
 * declared here rather than by pulling in node_api.h, which would want a
 * node checkout to build against.
 *
 * How they get resolved is the one thing that differs by platform, and
 * each way is the ordinary way for that platform:
 *
 *   - mach-o asks for them by name with a flat lookup, meaning whatever
 *     is already loaded, which is node. See test/ape/napi/exports.S.
 *   - windows has no such thing for an executable's own exports, so the
 *     addresses come from GetProcAddress on the running module, which is
 *     what node's own documentation suggests.
 *   - elf leaves them undefined and the loader binds them.
 */

#if SupportsWindows()
#define EXPORTED __attribute__((__ms_abi__))
#define NAPICALL __attribute__((__ms_abi__))
#else
#define EXPORTED
#define NAPICALL
#endif

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_callback_info__ *napi_callback_info;
typedef NAPICALL napi_value (*napi_callback)(napi_env, napi_callback_info);

// what node hands us, filled in by whichever of the three ways applies
NAPICALL int (*napi_create_int32_)(napi_env, int, napi_value *);
NAPICALL int (*napi_create_string_utf8_)(napi_env, const char *, size_t,
                                         napi_value *);
NAPICALL int (*napi_get_cb_info_)(napi_env, napi_callback_info, size_t *,
                                  napi_value *, napi_value *, void **);
NAPICALL int (*napi_get_value_int32_)(napi_env, napi_value, int *);
NAPICALL int (*napi_create_function_)(napi_env, const char *, size_t,
                                      napi_callback, void *, napi_value *);
NAPICALL int (*napi_set_named_property_)(napi_env, napi_value, const char *,
                                         napi_value);

#if SupportsWindows()
__msabi bool cosmo_dll_boot(void);
__msabi int cosmo_dll_thread_init(void);
#define enter_guest() cosmo_dll_thread_init()
#else
int cosmo_dylib_thread_init(void);
#define enter_guest() cosmo_dylib_thread_init()
#endif

/**
 * Adds two numbers, the long way round.
 *
 * Deliberately through the guest heap and the guest's own formatting, so
 * that what comes back is evidence the runtime is really running and not
 * just that the file loaded.
 */
static NAPICALL napi_value Add(napi_env env, napi_callback_info info) {
  enter_guest();
  size_t argc = 2;
  napi_value argv[2], out;
  napi_get_cb_info_(env, info, &argc, argv, 0, 0);
  int a = 0, b = 0;
  napi_get_value_int32_(env, argv[0], &a);
  napi_get_value_int32_(env, argv[1], &b);
  char *scratch = malloc(64);
  snprintf(scratch, 64, "%d", a + b);
  int sum = atoi(scratch);
  free(scratch);
  napi_create_int32_(env, sum, &out);
  return out;
}

/**
 * Reports the process from inside, using the guest's system calls.
 *
 * The pid is node's, since this is node's process, and javascript can
 * check that for itself.
 */
static NAPICALL napi_value Probe(napi_env env, napi_callback_info info) {
  enter_guest();
  static char msg[192];
  int n = snprintf(msg, sizeof(msg),
                   "cosmo libc in node: pid=%d tid=%d, heap and system calls "
                   "are the guest's",
                   getpid(), gettid());
  napi_value out;
  napi_create_string_utf8_(env, msg, n, &out);
  return out;
}

static void publish(napi_env env, napi_value exports, const char *name,
                    napi_callback fn) {
  napi_value value;
  napi_create_function_(env, name, strlen(name), fn, 0, &value);
  napi_set_named_property_(env, exports, name, value);
}

/**
 * What node calls once it has opened the file.
 */
EXPORTED napi_value napi_register_module_v1(napi_env env, napi_value exports) {
#if SupportsWindows()
  // the runtime came up when windows attached us, but say so anyway:
  // it costs a load and a branch, and it makes the order obvious
  cosmo_dll_boot();
  intptr_t node = GetModuleHandle(0);
#define BIND(f)                                              \
  do {                                                       \
    *(void **)&f##_ = (void *)GetProcAddress(node, #f);       \
    if (!f##_)                                               \
      return exports;                                        \
  } while (0)
  BIND(napi_create_int32);
  BIND(napi_create_string_utf8);
  BIND(napi_get_cb_info);
  BIND(napi_get_value_int32);
  BIND(napi_create_function);
  BIND(napi_set_named_property);
#undef BIND
#endif
  if (!napi_create_function_ || !napi_set_named_property_)
    return exports;
  publish(env, exports, "add", Add);
  publish(env, exports, "probe", Probe);
  return exports;
}
