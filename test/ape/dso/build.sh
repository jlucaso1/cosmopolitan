#!/bin/sh
# Builds the hosted shared object test.
#
#   test/ape/dso/build.sh
#   o/dsotest/host o/dsotest/cosmo_dso_test.so
#
# Unlike the PE and mach-o test next door, this one links against the
# whole libc, so it needs a build of it that can go in a shared object:
#
#   -fPIC        no absolute relocations in allocatable sections
#   COSMO_DSO    the assembly that can't express itself in pc relative
#                form takes its other path
#   tlscc        rewrites direct %fs accesses, the inline asm in
#                __get_tls() among them, into calls to the getters, which
#                is where hosted mode redirects thread local storage.
#                MODE=optlinux turns it off, so it's asked for by name
#   PKG          the package checker rejects symbols no declared
#                dependency defines, and this deliberately leaves
#                __tls_get_addr to the host's dynamic loader
#
# Set PICLIB to reuse a libc built earlier, which is worth doing since
# building it is most of the time here.

set -eu

# What to build. The test library by default; NAME and SRCS point it at
# something else, the way the mach-o and pe side does it.
NAME=${NAME:-cosmo_dso_test}
SRCS=${SRCS:-test/ape/dso/library.c}

COSMOCC=${COSMOCC:-.cosmocc/3.9.2}
OUT=${OUT:-o/dsotest}
MODE=optlinux

TLSCC=${TLSCC:-build/bootstrap/tlscc}
FIXUPOBJ=${FIXUPOBJ:-$COSMOCC/bin/fixupobj}
CC="$COSMOCC/bin/x86_64-linux-cosmo-gcc"
PICLIB=${PICLIB:-o/$MODE/cosmopolitan.a}

mkdir -p "$OUT"

if [ ! -f "$PICLIB" ]; then
  make -j"$(nproc)" MODE=$MODE \
       TLSCC="$TLSCC" \
       CONFIG_CCFLAGS+=-fPIC \
       CONFIG_CPPFLAGS+=-DCOSMO_DSO \
       PKG=test/ape/dso/package.sh \
       "o/$MODE/cosmopolitan.a"
fi

CFLAGS="-fPIC -DCOSMO_DSO -D_COSMO_SOURCE -DMODE=$MODE -DSUPPORT_VECTOR=1 \
        -DNDEBUG -DSYSDEBUG -nostdinc -iquote. -isystem libc/isystem \
        -include libc/integral/normalize.inc \
        -Wa,-W -Wa,-I. -Wa,--noexecstack \
        -Wall -Werror -Wno-prio-ctor-dtor -Wno-unknown-pragmas \
        -O2 -g -std=gnu23 -mavx -mno-red-zone -mno-tls-direct-seg-refs \
        -fno-common -fno-gnu-unique -fno-semantic-interposition"

MODOBJS=
for src in test/ape/dso/dso.c $SRCS; do
  obj="$OUT/$(basename "$src" | tr . _).o"
  # shellcheck disable=SC2086
  $TLSCC $CC $CFLAGS -c -o "$obj" "$src"
  $FIXUPOBJ "$obj"
  MODOBJS="$MODOBJS $obj"
done

# -Bsymbolic            lets the libc's own globals bind at link time
# --gc-sections         drops the windows import tables, which are
#                       absolute by nature and would block an ET_DYN link
# -init=cosmo_dso_noop  _init is ld's default DT_INIT and is not a
#                       function; see test/ape/dso/dso.c
${LD:-ld} -shared --gc-sections -Bsymbolic -init=cosmo_dso_noop \
    -T test/ape/dso/dso.lds -o "$OUT/$NAME.so" \
    $MODOBJS "$PICLIB"

[ -n "${NOHOST:-}" ] ||
  ${HOSTCC:-cc} -O2 -o "$OUT/host" test/ape/dso/host.c -ldl -lpthread

ls -l "$OUT/$NAME.so"
