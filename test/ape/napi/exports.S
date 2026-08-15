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
#include "ape/relocations.h"

//	What the addon publishes, and what it asks node for.
//
//	One name goes out, which is the one node looks for. What comes in
//	is the part of N-API this uses, asked for flat: whatever is already
//	loaded, which is the program doing the loading. That is the same
//	arrangement an addon built by any other toolchain gets from
//	-undefined dynamic_lookup.

	.export	napi_register_module_v1

#if SupportsXnu() && !SupportsWindows()
	.import	napi_create_function_,"_napi_create_function",APE_IMPORT_FLAT
	.import	napi_create_int32_,"_napi_create_int32",APE_IMPORT_FLAT
	.import	napi_create_string_utf8_,"_napi_create_string_utf8",APE_IMPORT_FLAT
	.import	napi_get_cb_info_,"_napi_get_cb_info",APE_IMPORT_FLAT
	.import	napi_get_value_int32_,"_napi_get_value_int32",APE_IMPORT_FLAT
	.import	napi_set_named_property_,"_napi_set_named_property",APE_IMPORT_FLAT
#endif
