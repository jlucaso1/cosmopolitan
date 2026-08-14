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
# The support vector is narrowed to one host on purpose. A library is
# loaded by a specific operating system, so there is nothing for the
# other hosts' startup code to do, and leaving it in only drags in the
# bare metal boot path.

set -eu

TARGET=${1:-}
COSMOCC=${COSMOCC:-.cosmocc/3.9.2}
OUT=${OUT:-o/dlltest}

case "$TARGET" in
  windows) VECTOR=4; SUFFIX=dll ;;
  macos)   VECTOR=8; SUFFIX=dylib ;;
  *) echo "usage: $0 windows|macos" >&2; exit 1 ;;
esac

CC="$COSMOCC/bin/x86_64-linux-cosmo-gcc"
LD="$COSMOCC/bin/x86_64-linux-cosmo-ld.bfd"
OBJCOPY="$COSMOCC/bin/x86_64-linux-cosmo-objcopy"

mkdir -p "$OUT"

CFLAGS="-DAPE_DLL -DSUPPORT_VECTOR=$VECTOR -D_COSMO_SOURCE \
        -nostdinc -iquote. -I. -isystem libc/isystem \
        -include libc/integral/normalize.inc \
        -O2 -fno-pie -mno-red-zone"

# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/ape.o" ape/ape.S
# shellcheck disable=SC2086
$CC $CFLAGS -c -o "$OUT/exports.o" test/ape/dll/exports.S
# shellcheck disable=SC2086
$CC $CFLAGS -std=gnu2x -c -o "$OUT/library.o" test/ape/dll/library.c

$CC -D__LINKER__ -DAPE_DLL -DSUPPORT_VECTOR=$VECTOR -D_COSMO_SOURCE \
    -E -P -xc -nostdinc -iquote. -I. -isystem libc/isystem \
    -o "$OUT/ape.lds" ape/ape.lds

$LD -static -nostdlib -no-pie -z noexecstack -z norelro --gc-sections \
    -T "$OUT/ape.lds" -o "$OUT/cosmo_dll_test.dbg" \
    "$OUT/ape.o" "$OUT/exports.o" "$OUT/library.o"

$OBJCOPY -S -O binary "$OUT/cosmo_dll_test.dbg" "$OUT/cosmo_dll_test.$SUFFIX"

# the export trie holds addresses as uleb128, which nothing before this
# point can encode
if [ "$TARGET" = macos ]; then
  python3 test/ape/dll/trieaddrs.py "$OUT/cosmo_dll_test.$SUFFIX"
fi

ls -l "$OUT/cosmo_dll_test.$SUFFIX"
