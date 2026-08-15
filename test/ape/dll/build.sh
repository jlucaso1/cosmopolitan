#!/bin/sh
# Builds the shared object test library.
#
#   test/ape/dll/build.sh windows   # -> cosmo_dll_test.dll
#   test/ape/dll/build.sh macos     # -> cosmo_dll_test.dylib
#   test/ape/dll/build.sh arm64     # -> cosmo_dll_test.dylib, apple silicon
#
# Cosmopolitan doesn't use a PE or mach-o linker: ape/ape.S writes those
# headers by hand and ape.lds lays them out, so a library is a matter of
# APE_DLL rather than a different toolchain. The image comes out of the
# elf link with objcopy, the same way a .com does.
#
# The windows library carries the whole libc, so that the test can ask it
# something only a running runtime can answer. That needs a build of the
# libc with every host still in it, since narrowing the support vector is
# a property of how the libc itself was compiled.
#
# The mach-o one is still on its own. A dylib gets slid by dyld, and
# nothing here emits the rebase information that would let the libc's
# absolute addresses survive that, so for now it stays freestanding.

set -eu

# What to build. The test library by default; NAME and SRCS point it at
# something else, which is how the node addon is built with the same
# steps rather than a second copy of them.
NAME=${NAME:-cosmo_dll_test}
SRCS=${SRCS:-"test/ape/dll/library.c test/ape/dll/exports.S test/ape/dll/exports2.S"}

# Objects and archives that were built elsewhere, for a library whose code
# this cannot compile: rust-ape's node addon hands over what cargo produced
# and lets the rest of these steps apply to it. UNDEF names symbols to keep,
# which an archive needs when nothing in the link refers to them -- an addon
# entry point is looked up by name after the fact, so without it the member
# holding it is never pulled in.
EXTRA=${EXTRA:-}
UNDEF=${UNDEF:-}

TARGET=${1:-}
COSMOCC=${COSMOCC:-.cosmocc/3.9.2}
OUT=${OUT:-o/dlltest}

# Each target has a mode of its own, because the libc it wants is not the
# same libc with a flag added: see the shared library modes in
# build/config.mk. Sharing an output tree between them would let make
# hand one target the objects it compiled for another.
case "$TARGET" in
  windows) VECTOR=; SUFFIX=dll;   BOOTSRC=libc/runtime/windllboot.c; PIC=
           ARCH=x86_64; MODE=dll; PAGE=4096 ;;
  macos)   VECTOR=8; SUFFIX=dylib; BOOTSRC=libc/runtime/dylibboot.c; PIC=-fPIC
           ARCH=x86_64; MODE=dylib; PAGE=4096 ;;
  arm64)   VECTOR=8; SUFFIX=dylib; BOOTSRC=libc/runtime/dylibboot.c; PIC=-fPIC
           ARCH=aarch64; MODE=aarch64-dylib; PAGE=16384 ;;
  *) echo "usage: $0 windows|macos|arm64" >&2; exit 1 ;;
esac

TLSCC=
REDZONE=
if [ "$ARCH" = x86_64 ]; then
  TLSCC=build/bootstrap/tlscc
  REDZONE=-mno-red-zone
fi

CC="$COSMOCC/bin/$ARCH-linux-cosmo-gcc"
LD="$COSMOCC/bin/$ARCH-linux-cosmo-ld.bfd"
OBJCOPY="$COSMOCC/bin/$ARCH-linux-cosmo-objcopy"

mkdir -p "$OUT"

# The mach-o side needs a libc built as position independent code, since
# dyld slides a library wherever it likes and an absolute address in the
# text would land in whatever happens to be there. The mode says so, and
# the mode is also what names the output tree, so this can hand the work
# to make unconditionally and let it decide what's already built.
VECTORFLAG=${VECTOR:+-DSUPPORT_VECTOR=$VECTOR}
LIBC=${LIBC_A:-o/$MODE/cosmopolitan.a}
if [ -z "${LIBC_A:-}" ]; then
  # tlscc rewrites direct %fs accesses, which is an x86 concern; the
  # other architecture keeps its thread pointer in a register
  make -j"$(nproc)" MODE=$MODE \
       ${TLSCC:+TLSCC=$TLSCC} \
       ${PIC:+PKG=test/ape/dso/package.sh} "$LIBC"
fi

CFLAGS="-DAPE_DLL $VECTORFLAG -D_COSMO_SOURCE ${PIC:--fno-pie} \
        ${PIC:+-DCOSMO_DSO} \
        -nostdinc -iquote. -I. -isystem libc/isystem \
        -include libc/integral/normalize.inc \
        -O2 ${REDZONE} ${EXTRA_CFLAGS:-}"

# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/ape.o" ape/ape.S
MODOBJS=
for src in $SRCS; do
  obj="$OUT/$(basename "$src" | tr . _).o"
  case "$src" in
    *.c) # shellcheck disable=SC2086
         $CC $CFLAGS -std=gnu2x -c -o "$obj" "$src" ;;
    *)   # shellcheck disable=SC2086
         $CC $CFLAGS -c -o "$obj" "$src" ;;
  esac
  MODOBJS="$MODOBJS $obj"
done

# shellcheck disable=SC2086
$CC $CFLAGS -std=gnu2x -c -o "$OUT/boot.o" "$BOOTSRC"
BOOT="$OUT/boot.o"

# the thunks that hand the thread pointer back and forth are only a
# thing where the host might be using the register it lives in
THUNK=
if [ "$ARCH" = aarch64 ]; then
  # shellcheck disable=SC2086
  $CC $CFLAGS -c -o "$OUT/thunk.o" ape/dylibthunk.S
  THUNK="$OUT/thunk.o"
fi

$CC -D__LINKER__ -DAPE_DLL $VECTORFLAG -D_COSMO_SOURCE \
    -E -P -xc -nostdinc -iquote. -I. -isystem libc/isystem \
    -o "$OUT/ape.lds" ape/ape.lds

# --emit-relocs keeps the records of every absolute address the linker
# resolved, which is what the mach-o rebase information is built from.
#
# --no-relax stops it from turning a reference through the global offset
# table into an absolute immediate, which it is entitled to do in a
# static link at a fixed address, and which nothing can relocate
# afterwards: an address inside an instruction is not a word dyld can
# slide. Kept in the table, it is.
# apple silicon has bigger pages, and dyld will not map a segment that
# doesn't start on one
UNDEFARGS=
for sym in $UNDEF; do
  UNDEFARGS="$UNDEFARGS --undefined=$sym"
done

# shellcheck disable=SC2086
$LD -static -nostdlib -no-pie -z noexecstack -z norelro \
    -z common-page-size=$PAGE -z max-page-size=$PAGE --gc-sections \
    ${PIC:+--emit-relocs --no-relax --undefined=cosmo_dylib_routine} \
    $UNDEFARGS \
    -T "$OUT/ape.lds" -o "$OUT/$NAME.dbg" \
    "$OUT/ape.o" $MODOBJS $BOOT $THUNK $EXTRA $LIBC

$OBJCOPY -S -O binary "$OUT/$NAME.dbg" "$OUT/$NAME.$SUFFIX"

# the export trie, the rebase opcodes and the binds all hold what the
# link only just decided, in encodings no relocation can carry
if [ "$SUFFIX" = dylib ]; then
  # built with the toolchain rather than through the build, so that it
  # doesn't share an output tree with a libc compiled for a library
  APEDYLIB=${APEDYLIB:-$OUT/apedylib}
  [ -x "$APEDYLIB" ] ||
    "$COSMOCC/bin/cosmocc" -I. -O2 -o "$APEDYLIB" tool/build/apedylib.c
  "$APEDYLIB" "$OUT/$NAME.$SUFFIX" "$OUT/$NAME.dbg"
fi

ls -l "$OUT/$NAME.$SUFFIX"
