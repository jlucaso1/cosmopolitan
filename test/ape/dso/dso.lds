/*-*- mode: ld-script; indent-tabs-mode: nil; tab-width: 2; coding: utf-8   -*-│
│ vi: set et sts=2 sw=2 fenc=utf-8                                         :vi │
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

/* Linking cosmopolitan libc into an ELF shared object.
 *
 * This adds to the default -shared script rather than replacing it: the
 * dynamic loader is the host's, so the parts ape.lds hand writes are none
 * of our business here. What ld's own script doesn't know about is the
 * decentralized sections _init() is assembled from.
 *
 * _init is not a function. The linker builds it out of fragments spread
 * across the libc, and it relies on three invariants:
 *
 *   1. .initprologue, the .init.<priority>.<name> fragments sorted by
 *      name, and .initepilogue have to be contiguous and in that order,
 *      because each fragment falls through into the next.
 *
 *   2. %rdi walks .piro.bss.init.* in monotonic lockstep, from
 *      __init_bss_start to __init_bss_end, with each fragment's data in
 *      between. Sorting by name is what lines a fragment up with its own
 *      slice of bss.
 *
 *   3. Same for %rsi over .initro.*.
 *
 * Left to link order the two pointers drift away from their data and the
 * first system call jumps into whatever follows.
 *
 * @see ape/ape.lds
 * @see libc/runtime/init.S
 * @see libc/macros.h
 */

/* __maps_init() registers the image using these. Under -shared the
 * default script sets __executable_start to zero and leaves _rodata
 * undefined, which would have cosmopolitan claim 140 TB and wedge
 * __maps_mark(). _etext and _end it already gets right. */
__executable_start = ADDR(.cosmo.init);
_rodata = ADDR(.rodata);

/* Protection of the main stack, PROT_READ|PROT_WRITE, given as an address
 * the way ape.lds gives it. Undefined, __maps_adder() sees no bits set and
 * takes the whole stack back out of the kisdangerous() trie, after which
 * every read() into a stack buffer comes back EFAULT. */
ape_stack_prot = 3;

SECTIONS {
  .cosmo.init : {
    KEEP(*(.initprologue))
    KEEP(*(SORT_BY_NAME(.init.*)))
    KEEP(*(.initepilogue))
  }
} INSERT BEFORE .text;

SECTIONS {
  .cosmo.initro : {
    . = ALIGN(8);
    KEEP(*(.initroprologue))
    KEEP(*(SORT_BY_NAME(.initro.*)))
    KEEP(*(.initroepilogue))
    KEEP(*(SORT_BY_NAME(.sort.rodata.*)))
  }
} INSERT AFTER .rodata;

/* Cosmopolitan's constructors assume _init() already ran, several of them
 * making system calls, and __systemfive only learns where to dispatch in
 * _init(). The loader runs DT_INIT_ARRAY at dlopen time, well before that,
 * so the constructors move into a vector of our own for cosmo_dso_init()
 * to run at the right moment. */
SECTIONS {
  .cosmo.init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array))
    KEEP(*(.ctors))
    __init_array_end = .;
  }
} INSERT BEFORE .init_array;

SECTIONS {
  .cosmo.initbss (NOLOAD) : {
    . = ALIGN(8);
    KEEP(*(.bssprologue))
    KEEP(*(SORT_BY_NAME(.piro.bss.init.*)))
    KEEP(*(SORT_BY_NAME(.piro.bss.sort.*)))
    *(.piro.bss)
  }
} INSERT BEFORE .bss;
