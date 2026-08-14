#!/usr/bin/env python3
"""Writes export addresses into a mach-o export trie.

A trie stores addresses as uleb128, whose bytes depend on the value, so
nothing in an object file can hold one that isn't known until the link
is done. ape/relocations.h leaves each node a five byte hole instead,
and this fills it, taking the addresses from the symbol table that's
already in the image.

Upstream this belongs in apelink, next to the rest of the mach-o
arithmetic. It's a script here because the shared object test builds
with the toolchain alone and no build system around it.

    trieaddrs.py cosmo_dll_test.dylib
"""

import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
LC_DYLD_EXPORTS_TRIE = 0x80000033

NLIST_SIZE = 16
ADDRESS_HOLE = 5


def read_cstring(image, pos):
    end = image.index(b"\0", pos)
    return image[pos:end].decode(), end + 1


def read_uleb(image, pos):
    value = shift = 0
    while True:
        byte = image[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def write_uleb(image, pos, value, width):
    """Writes a uleb128 of a fixed width, padding with empty groups."""
    for i in range(width):
        byte = value & 0x7F
        value >>= 7
        image[pos + i] = byte | (0x80 if i + 1 < width else 0)
    if value:
        raise ValueError("address needs more than %d bytes" % width)


def parse(image):
    magic, _, _, _, ncmds, _, _ = struct.unpack_from("<7I", image, 0)
    if magic != MH_MAGIC_64:
        raise ValueError("not a 64 bit mach-o")
    symtab = trie = None
    base = None
    pos = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", image, pos)
        if cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<4I", image, pos + 8)
        elif cmd == LC_DYLD_EXPORTS_TRIE:
            trie = struct.unpack_from("<2I", image, pos + 8)
        elif cmd == LC_SEGMENT_64:
            name = image[pos + 8 : pos + 24].rstrip(b"\0")
            if name == b"__TEXT":
                base = struct.unpack_from("<Q", image, pos + 24)[0]
        pos += cmdsize
    if symtab is None or trie is None or base is None:
        raise ValueError("missing symtab, export trie, or __TEXT")
    return symtab, trie, base


def symbol_addresses(image, symtab):
    symoff, nsyms, stroff, _ = symtab
    addresses = {}
    for i in range(nsyms):
        strx, _, _, _, value = struct.unpack_from(
            "<IBBHQ", image, symoff + i * NLIST_SIZE
        )
        name, _ = read_cstring(image, stroff + strx)
        addresses[name] = value
    return addresses


def main(path):
    with open(path, "rb") as f:
        image = bytearray(f.read())

    symtab, (trieoff, _), base = parse(image)
    addresses = symbol_addresses(image, symtab)

    # the root node: no value of its own, then one edge per export
    pos = trieoff
    terminal, pos = read_uleb(image, pos)
    pos += terminal
    nchildren = image[pos]
    pos += 1

    for _ in range(nchildren):
        name, pos = read_cstring(image, pos)
        node, pos = read_uleb(image, pos)
        if name not in addresses:
            raise ValueError("%s is in the trie but not the symbol table" % name)
        # the node holds a terminal size, then flags, then the address
        at = trieoff + node
        _, at = read_uleb(image, at)
        _, at = read_uleb(image, at)
        write_uleb(image, at, addresses[name] - base, ADDRESS_HOLE)
        print("%s at %#x" % (name, addresses[name] - base))

    with open(path, "wb") as f:
        f.write(image)


if __name__ == "__main__":
    main(sys.argv[1])
