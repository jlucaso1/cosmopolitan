#!/usr/bin/env python3
"""Finishes a mach-o image that the elf linker just produced.

Two things can't be written until the link is done, and neither can be
expressed as a relocation:

  - the address in each export trie node, which is uleb128, so its bytes
    depend on the value;

  - the rebase opcodes, which tell dyld which words in the image hold
    addresses. dyld puts a library wherever it likes, and everything the
    linker resolved absolutely has to move with it.

ape/ape.S leaves a hole for each. The addresses come out of the symbol
table that's already in the image; the rebase list comes from the
relocation records the link was told to keep with --emit-relocs.

Upstream this belongs in apelink, next to the rest of the mach-o
arithmetic. It's a script here because the shared object test builds with
the toolchain alone and no build system around it.

    machofix.py cosmo_dll_test.dylib cosmo_dll_test.dbg
"""

import os
import re
import struct
import subprocess
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
LC_DYLD_INFO_ONLY = 0x80000022

NLIST_SIZE = 16
ADDRESS_HOLE = 5

REBASE_TYPE_POINTER = 1
REBASE_OPCODE_DONE = 0x00
REBASE_OPCODE_SET_TYPE_IMM = 0x10
REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20
REBASE_OPCODE_ADD_ADDR_ULEB = 0x30
REBASE_OPCODE_DO_REBASE_ULEB_TIMES = 0x60

POINTER_SIZE = 8


class Segment:
    def __init__(self, index, name, vmaddr, vmsize, fileoff, filesize):
        self.index = index
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize

    def holds(self, addr):
        return self.vmaddr <= addr < self.vmaddr + self.vmsize


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


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


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
    segments = []
    symtab = None
    dyld_info = None
    dyld_info_at = None
    pos = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", image, pos)
        if cmd == LC_SEGMENT_64:
            name = image[pos + 8 : pos + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                "<4Q", image, pos + 24
            )
            segments.append(
                Segment(len(segments), name, vmaddr, vmsize, fileoff, filesize)
            )
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<4I", image, pos + 8)
        elif cmd == LC_DYLD_INFO_ONLY:
            dyld_info = struct.unpack_from("<10I", image, pos + 8)
            dyld_info_at = pos + 8
        pos += cmdsize
    if symtab is None or dyld_info is None or not segments:
        raise ValueError("missing segments, symtab, or dyld info")
    return segments, symtab, dyld_info, dyld_info_at, 32 + struct.unpack_from(
        "<I", image, 20
    )[0]


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


def fill_export_addresses(image, trieoff, base, addresses):
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
        at = trieoff + node
        _, at = read_uleb(image, at)
        _, at = read_uleb(image, at)
        write_uleb(image, at, addresses[name] - base, ADDRESS_HOLE)
        print("export %s at %#x" % (name, addresses[name] - base))


def absolute_words(debug, segments, header_end):
    """Every word the linker resolved to an address inside the image.

    The relocation records say where they are. What has to be left out is
    the parts of the image that describe it rather than run: dyld reads
    the header and __LINKEDIT before it slides anything, and expects the
    addresses in them to be the ones the linker wrote.
    """
    linkedit = next((s for s in segments if s.name == "__LINKEDIT"), None)
    out = subprocess.run(
        ["readelf", "-rW", debug],
        check=True,
        capture_output=True,
        env=dict(os.environ, LC_ALL="C"),
    ).stdout.decode(errors="replace")
    section = None
    found = set()
    skipped = 0
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            section = m.group(1)
            continue
        if not section or "R_X86_64_64" not in line:
            continue
        if section.startswith(".rela.debug") or section.startswith(".rela.eh"):
            continue
        addr = int(line.split()[0], 16)
        if addr % POINTER_SIZE:
            skipped += 1  # dyld can only rebase aligned words
            continue
        if addr < header_end:
            continue  # the header describes the image
        if linkedit and linkedit.holds(addr):
            continue  # so does __LINKEDIT
        if not any(s.holds(addr) for s in segments):
            continue
        found.add(addr)
    if skipped:
        print("warning: %d unaligned absolute words left alone" % skipped)
    return sorted(found)


def rebase_opcodes(addrs, segments):
    out = bytearray()
    out.append(REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER)
    seg = None
    cur = 0
    i = 0
    while i < len(addrs):
        addr = addrs[i]
        here = next(s for s in segments if s.holds(addr))
        if here is not seg:
            seg = here
            out.append(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg.index)
            out += uleb(addr - seg.vmaddr)
            cur = addr
        elif addr != cur:
            out.append(REBASE_OPCODE_ADD_ADDR_ULEB)
            out += uleb(addr - cur)
            cur = addr
        run = 1
        while i + run < len(addrs) and addrs[i + run] == addr + run * POINTER_SIZE:
            run += 1
        out.append(REBASE_OPCODE_DO_REBASE_ULEB_TIMES)
        out += uleb(run)
        cur = addr + run * POINTER_SIZE
        i += run
    out.append(REBASE_OPCODE_DONE)
    return bytes(out)


def main(path, debug):
    with open(path, "rb") as f:
        image = bytearray(f.read())

    segments, symtab, dyld_info, dyld_info_at, header_end = parse(image)
    base = segments[0].vmaddr
    rebase_off, rebase_size = dyld_info[0], dyld_info[1]
    trieoff = dyld_info[8]

    fill_export_addresses(image, trieoff, base, symbol_addresses(image, symtab))

    addrs = absolute_words(debug, segments, header_end)
    opcodes = rebase_opcodes(addrs, segments)
    if len(opcodes) > rebase_size:
        raise ValueError(
            "rebase opcodes need %d bytes and only %d were set aside; raise "
            "APE_MACHO_REBASE_SIZE" % (len(opcodes), rebase_size)
        )
    image[rebase_off : rebase_off + len(opcodes)] = opcodes

    # dyld wants the size to be what the opcodes actually take, not the
    # room they were given, and says so if the stream stops early
    struct.pack_into("<I", image, dyld_info_at + 4, len(opcodes))

    where = {}
    for addr in addrs:
        seg = next(s for s in segments if s.holds(addr))
        where[seg.name] = where.get(seg.name, 0) + 1
    print(
        "rebase %d addresses in %d bytes: %s"
        % (
            len(addrs),
            len(opcodes),
            ", ".join("%s %d" % kv for kv in sorted(where.items())),
        )
    )

    # a segment that claims more of the file than the file has is a bus
    # error the moment dyld touches the end of it
    for seg in segments:
        end = seg.fileoff + seg.filesize
        if end > len(image):
            raise ValueError(
                "%s runs to %#x and the file stops at %#x"
                % (seg.name, end, len(image))
            )
        print(
            "segment %-12s file %#x..%#x  vm %#x..%#x"
            % (seg.name, seg.fileoff, end, seg.vmaddr, seg.vmaddr + seg.vmsize)
        )

    with open(path, "wb") as f:
        f.write(image)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
