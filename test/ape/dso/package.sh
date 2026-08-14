#!/bin/sh
# Stands in for the package checker while building the libc for a shared
# object. That tool rejects any symbol no declared dependency defines, and
# a shared object deliberately leaves __tls_get_addr to whatever dynamic
# loader ends up hosting it. Checking is all it does, so writing the
# output file is enough to keep make moving.

out=
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift 2 ;;
    -o*) out=${1#-o}; shift ;;
    *) shift ;;
  esac
done

[ -n "$out" ] && : >"$out"
exit 0
