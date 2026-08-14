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
  windows) VECTOR=; SUFFIX=dll ;;
  macos)   VECTOR=8; SUFFIX=dylib ;;
  *) echo "usage: $0 windows|macos" >&2; exit 1 ;;
esac

CC="$COSMOCC/bin/x86_64-linux-cosmo-gcc"
LD="$COSMOCC/bin/x86_64-linux-cosmo-ld.bfd"
OBJCOPY="$COSMOCC/bin/x86_64-linux-cosmo-objcopy"

mkdir -p "$OUT"

LIBC=
if [ -z "$VECTOR" ]; then
  LIBC=${LIBC_A:-o//cosmopolitan.a}
  [ -f "$LIBC" ] || make -j"$(nproc)" MODE= "$LIBC"
  VECTORFLAG=
else
  VECTORFLAG=-DSUPPORT_VECTOR=$VECTOR
fi

CFLAGS="-DAPE_DLL $VECTORFLAG -D_COSMO_SOURCE \
        -nostdinc -iquote. -I. -isystem libc/isystem \
        -include libc/integral/normalize.inc \
        -O2 -fno-pie -mno-red-zone"

# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/ape.o" ape/ape.S
# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/exports.o" test/ape/dll/exports.S
# shellcheck disable=SC2086
$CC $CFLAGS -std=gnu2x -c -o "$OUT/library.o" test/ape/dll/library.c

BOOT=
if [ -n "$LIBC" ]; then
  # shellcheck disable=SC2086
  $CC $CFLAGS -std=gnu2x -c -o "$OUT/boot.o" libc/runtime/windllboot.c
  BOOT="$OUT/boot.o"
fi

$CC -D__LINKER__ -DAPE_DLL $VECTORFLAG -D_COSMO_SOURCE \
    -E -P -xc -nostdinc -iquote. -I. -isystem libc/isystem \
    -o "$OUT/ape.lds" ape/ape.lds

$LD -static -nostdlib -no-pie -z noexecstack -z norelro --gc-sections \
    -T "$OUT/ape.lds" -o "$OUT/cosmo_dll_test.dbg" \
    "$OUT/ape.o" "$OUT/exports.o" "$OUT/library.o" $BOOT $LIBC

$OBJCOPY -S -O binary "$OUT/cosmo_dll_test.dbg" "$OUT/cosmo_dll_test.$SUFFIX"

# the export trie holds addresses as uleb128, which nothing before this
# point can encode
if [ "$TARGET" = macos ]; then
  python3 test/ape/dll/trieaddrs.py "$OUT/cosmo_dll_test.$SUFFIX"
fi

ls -l "$OUT/cosmo_dll_test.$SUFFIX"
