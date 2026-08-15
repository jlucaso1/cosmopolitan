#!/bin/sh
# Builds the shared object test library.
#
#   test/ape/dll/build.sh windows   # -> cosmo_dll_test.dll
#   test/ape/dll/build.sh macos     # -> cosmo_dll_test.dylib
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

TARGET=${1:-}
COSMOCC=${COSMOCC:-.cosmocc/3.9.2}
OUT=${OUT:-o/dlltest}

case "$TARGET" in
  windows) VECTOR=; SUFFIX=dll;   BOOTSRC=libc/runtime/windllboot.c; PIC= ;;
  macos)   VECTOR=8; SUFFIX=dylib; BOOTSRC=libc/runtime/dylibboot.c; PIC=-fPIC ;;
  *) echo "usage: $0 windows|macos" >&2; exit 1 ;;
esac

CC="$COSMOCC/bin/x86_64-linux-cosmo-gcc"
LD="$COSMOCC/bin/x86_64-linux-cosmo-ld.bfd"
OBJCOPY="$COSMOCC/bin/x86_64-linux-cosmo-objcopy"

mkdir -p "$OUT"

# The mach-o side needs a libc built as position independent code, since
# dyld slides a library wherever it likes and an absolute address in the
# text would land in whatever happens to be there.
VECTORFLAG=${VECTOR:+-DSUPPORT_VECTOR=$VECTOR}
LIBC=${LIBC_A:-o//cosmopolitan.a}
if [ ! -f "$LIBC" ]; then
  if [ -n "$PIC" ]; then
    make -j"$(nproc)" MODE= TLSCC=build/bootstrap/tlscc \
         CONFIG_CCFLAGS+=-fPIC \
         "CONFIG_CPPFLAGS+=-DCOSMO_DSO $VECTORFLAG" \
         PKG=test/ape/dso/package.sh "$LIBC"
  else
    make -j"$(nproc)" MODE= "$LIBC"
  fi
fi

CFLAGS="-DAPE_DLL $VECTORFLAG -D_COSMO_SOURCE ${PIC:--fno-pie} \
        ${PIC:+-DCOSMO_DSO} \
        -nostdinc -iquote. -I. -isystem libc/isystem \
        -include libc/integral/normalize.inc \
        -O2 -mno-red-zone"

# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/ape.o" ape/ape.S
# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/exports.o" test/ape/dll/exports.S
# shellcheck disable=SC2086
$CC $CFLAGS -std=gnu2x -c -o "$OUT/library.o" test/ape/dll/library.c

# shellcheck disable=SC2086
$CC $CFLAGS -std=gnu2x -c -o "$OUT/boot.o" "$BOOTSRC"
BOOT="$OUT/boot.o"

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
$LD -static -nostdlib -no-pie -z noexecstack -z norelro --gc-sections \
    ${PIC:+--emit-relocs --no-relax --undefined=cosmo_dylib_routine} \
    -T "$OUT/ape.lds" -o "$OUT/cosmo_dll_test.dbg" \
    "$OUT/ape.o" "$OUT/exports.o" "$OUT/library.o" $BOOT $LIBC

$OBJCOPY -S -O binary "$OUT/cosmo_dll_test.dbg" "$OUT/cosmo_dll_test.$SUFFIX"

# the export trie and the rebase opcodes both hold what the link only
# just decided, in encodings no relocation can carry
if [ "$TARGET" = macos ]; then
  python3 test/ape/dll/machofix.py "$OUT/cosmo_dll_test.$SUFFIX" \
      "$OUT/cosmo_dll_test.dbg"
fi

ls -l "$OUT/cosmo_dll_test.$SUFFIX"
