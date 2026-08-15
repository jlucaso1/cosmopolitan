/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
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
#include "libc/calls/calls.h"
#include "libc/calls/struct/stat.h"
#include "libc/elf/def.h"
#include "libc/elf/scalar.h"
#include "libc/elf/struct/ehdr.h"
#include "libc/elf/struct/rela.h"
#include "libc/elf/struct/shdr.h"
#include "libc/elf/struct/sym.h"
#include "libc/fmt/libgen.h"
#include "libc/macho.h"
#include "libc/macros.h"
#include "libc/mem/mem.h"
#include "libc/mem/alg.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/o.h"
#include "libc/x/x.h"

/**
 * @fileoverview Finishes a mach-o library the elf linker just produced.
 *
 * Three of the things a mach-o carries can't be written by a linker that
 * doesn't know what a mach-o is, and can't be spelled as relocations
 * either, because all three are uleb128 and the width of a uleb128
 * depends on what it encodes:
 *
 *   - the export trie, which is what dlsym() reads;
 *   - the rebase opcodes, which say which words hold addresses that
 *     have to move when dyld puts the library somewhere;
 *   - the bind opcodes, which say which words dyld should fill in with
 *     addresses from somewhere else.
 *
 * ape/ape.S leaves a hole for each. What goes in them comes from the
 * image itself: the symbol table for the exports, the relocation records
 * the link was told to keep for the rebases, and a table the .import
 * directive builds for the binds.
 *
 *     apedylib cosmo.dylib cosmo.dylib.dbg
 */

#define POINTER_SIZE 8

#define REBASE_TYPE_POINTER                       1
#define REBASE_OPCODE_DONE                        0x00
#define REBASE_OPCODE_SET_TYPE_IMM                0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB               0x30
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES        0x60

#define BIND_TYPE_POINTER                         1
#define BIND_OPCODE_DONE                          0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM         0x10
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM         0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM                  0x50
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB   0x70
#define BIND_OPCODE_DO_BIND                       0x90

// what the .import directive lays down, one per symbol
struct Import {
  uint64_t slot;   // where the address goes, as a virtual address
  uint64_t flags;  // APE_IMPORT_* below
  char name[APE_MACHO_IMPORT_STRIDE - 16];
};

// mach-o's symbol table entry, which its own header doesn't spell out
struct Nlist {
  uint32_t strx;
  uint8_t type;
  uint8_t sect;
  uint16_t desc;
  uint64_t value;
};

struct Segment {
  int index;
  char name[17];
  uint64_t vmaddr, vmsize, fileoff, filesize;
  uint32_t initprot;
};

static const char *prog;
static char *image;
static size_t imagesize;
static char *debug;
static size_t debugsize;

static struct Segment segments[8];
static int nsegments;
static struct MachoDyldInfoCommand *dyldinfo;
static struct MachoLoadSymtab *symtab;
static uint64_t textbegins;

static wontreturn void die(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "%s: ", prog);
  vfprintf(stderr, fmt, va);
  fputc('\n', stderr);
  va_end(va);
  exit(1);
}

static char *slurp(const char *path, size_t *out_size) {
  int fd;
  struct stat st;
  char *data;
  if ((fd = open(path, O_RDONLY)) == -1 || fstat(fd, &st))
    die("%s: could not open", path);
  data = malloc(st.st_size);
  if (!data || read(fd, data, st.st_size) != st.st_size)
    die("%s: could not read", path);
  close(fd);
  *out_size = st.st_size;
  return data;
}

static size_t put_uleb(unsigned char *p, uint64_t value) {
  size_t n = 0;
  do {
    unsigned char byte = value & 0x7f;
    value >>= 7;
    p[n++] = byte | (value ? 0x80 : 0);
  } while (value);
  return n;
}

// a uleb128 of a chosen width, so that what it encodes can be worked out
// before it's known. redundant, and every reader accepts one.
static size_t put_uleb_fixed(unsigned char *p, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    p[i] = (value & 0x7f) | (i + 1 < width ? 0x80 : 0);
    value >>= 7;
  }
  if (value)
    die("value needs more than %zu bytes of uleb128", width);
  return width;
}

static struct Segment *segment_of(uint64_t addr) {
  for (int i = 0; i < nsegments; ++i)
    if (segments[i].vmaddr <= addr && addr < segments[i].vmaddr + segments[i].vmsize)
      return &segments[i];
  return 0;
}

static void *at_vaddr(uint64_t addr) {
  struct Segment *seg = segment_of(addr);
  if (!seg || addr - seg->vmaddr >= seg->filesize)
    return 0;
  return image + seg->fileoff + (addr - seg->vmaddr);
}

static void parse_macho(void) {
  struct MachoHeader *mh = (struct MachoHeader *)image;
  if (imagesize < 32 || mh->magic != 0xFEEDFACF)
    die("not a 64 bit mach-o");
  char *p = image + 32;
  for (uint32_t i = 0; i < mh->loadcount; ++i) {
    struct MachoLoadCommand *lc = (struct MachoLoadCommand *)p;
    if (lc->command == MAC_LC_SEGMENT_64) {
      struct MachoLoadSegment *sg = (struct MachoLoadSegment *)p;
      if (nsegments == ARRAYLEN(segments))
        die("more segments than expected");
      struct Segment *s = &segments[nsegments];
      s->index = nsegments++;
      memcpy(s->name, sg->name, 16);
      s->name[16] = 0;
      s->vmaddr = sg->vaddr;
      s->vmsize = sg->memsz;
      s->fileoff = sg->offset;
      s->filesize = sg->filesz;
      s->initprot = sg->initprot;
      // the headers describe the image rather than run, and the section
      // record for the code is where they stop
      struct MachoSection *sec = (struct MachoSection *)(p + 72);
      for (uint32_t j = 0; j < sg->sectioncount; ++j, ++sec)
        if (!strncmp(sec->name, "__text", 16))
          textbegins = sec->vaddr;
    } else if (lc->command == MAC_LC_SYMTAB) {
      symtab = (struct MachoLoadSymtab *)p;
    } else if (lc->command == MAC_LC_DYLD_INFO_ONLY) {
      dyldinfo = (struct MachoDyldInfoCommand *)p;
    }
    p += lc->size;
  }
  if (!symtab || !dyldinfo || !nsegments)
    die("missing segments, symbol table, or dyld info");
  if (!textbegins)
    die("no __text section record to bound the headers with");
}

////////////////////////////////////////////////////////////////////////////////
// the export trie

struct Export {
  const char *name;
  uint64_t addr;
};

static int compare_exports(const void *a, const void *b) {
  return strcmp(((const struct Export *)a)->name,
                ((const struct Export *)b)->name);
}

/**
 * Builds the trie dlsym() walks.
 *
 * A root with one edge per symbol and a node on the end of each. No
 * prefix sharing: a library exports a handful of names, and what the
 * shape has to be is correct, not small.
 *
 * The edges carry the offset of the node they lead to, which isn't known
 * until every edge has been laid down, so those are written at a fixed
 * width and the arithmetic comes out the same either way.
 */
static size_t build_trie(unsigned char *out, size_t room, uint64_t base) {
  int n = 0;
  struct Export *exports = malloc(symtab->count * sizeof(struct Export));
  struct Nlist *syms = (struct Nlist *)(image + symtab->offset);
  const char *strs = image + symtab->stroff;

  // The strings are fixed width, so which one belongs to an entry falls
  // out of where the entry is. Saying so here rather than in the .export
  // directive is what lets exports be written in more than one file: an
  // assembler can only count what it can see.
  for (uint32_t i = 0; i < symtab->count; ++i)
    syms[i].strx = 1 + i * MACHO_STRTAB_STRIDE;
  for (uint32_t i = 0; i < symtab->count; ++i) {
    if (!(syms[i].type & 1))  // N_EXT
      continue;
    exports[n].name = strs + syms[i].strx;
    exports[n].addr = syms[i].value;
    ++n;
  }
  qsort(exports, n, sizeof(struct Export), compare_exports);

#define EDGE_OFFSET_WIDTH 2
#define NODE_SIZE         8
  size_t rootsize = 2;  // no value of its own, then the number of edges
  for (int i = 0; i < n; ++i)
    rootsize += strlen(exports[i].name) + 1 + EDGE_OFFSET_WIDTH;
  size_t total = rootsize + (size_t)n * NODE_SIZE;
  if (total > room)
    die("the export trie needs %zu bytes and %zu were set aside; raise "
        "APE_MACHO_TRIE_SIZE",
        total, room);

  unsigned char *p = out;
  *p++ = 0;  // the root exports nothing itself
  if (n > 127)
    die("more exports than this writes a root for");
  *p++ = n;
  for (int i = 0; i < n; ++i) {
    size_t len = strlen(exports[i].name) + 1;
    memcpy(p, exports[i].name, len);
    p += len;
    p += put_uleb_fixed(p, rootsize + (size_t)i * NODE_SIZE, EDGE_OFFSET_WIDTH);
  }
  for (int i = 0; i < n; ++i) {
    unsigned char *node = out + rootsize + (size_t)i * NODE_SIZE;
    node[0] = 6;  // what the flags and the address take
    node[1] = 0;  // a regular export
    put_uleb_fixed(node + 2, exports[i].addr - base, 5);
    node[7] = 0;  // no children
    printf("export %s at %#lx\n", exports[i].name, exports[i].addr - base);
  }
  free(exports);
  return total;
}

////////////////////////////////////////////////////////////////////////////////
// the rebase opcodes

static uint64_t *rebases;
static size_t nrebases;

static void want_rebase(uint64_t addr) {
  struct Segment *seg;
  if (addr % POINTER_SIZE)
    return;  // dyld only rebases aligned words
  if (addr < textbegins)
    return;  // the headers describe the image, they don't run
  if (!(seg = segment_of(addr)))
    return;
  if (!strcmp(seg->name, "__LINKEDIT"))
    return;  // dyld reads this before it slides anything
  if (!(seg->initprot & 2))
    return;  // and won't write where it isn't allowed to
  for (size_t i = 0; i < nrebases; ++i)
    if (rebases[i] == addr)
      return;
  rebases = realloc(rebases, (nrebases + 1) * sizeof(uint64_t));
  rebases[nrebases++] = addr;
}

static int compare_addresses(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static size_t build_rebase(unsigned char *out, size_t room) {
  unsigned char *p = out;
  qsort(rebases, nrebases, sizeof(uint64_t), compare_addresses);
  *p++ = REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER;
  struct Segment *seg = 0;
  uint64_t cursor = 0;
  for (size_t i = 0; i < nrebases;) {
    struct Segment *here = segment_of(rebases[i]);
    if (here != seg) {
      seg = here;
      *p++ = REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg->index;
      p += put_uleb(p, rebases[i] - seg->vmaddr);
      cursor = rebases[i];
    } else if (rebases[i] != cursor) {
      *p++ = REBASE_OPCODE_ADD_ADDR_ULEB;
      p += put_uleb(p, rebases[i] - cursor);
      cursor = rebases[i];
    }
    size_t run = 1;
    while (i + run < nrebases &&
           rebases[i + run] == rebases[i] + run * POINTER_SIZE)
      ++run;
    *p++ = REBASE_OPCODE_DO_REBASE_ULEB_TIMES;
    p += put_uleb(p, run);
    cursor = rebases[i] + run * POINTER_SIZE;
    i += run;
    if ((size_t)(p - out) + 32 > room)
      die("the rebase opcodes need more than the %zu bytes set aside; raise "
          "APE_MACHO_REBASE_SIZE",
          room);
  }
  *p++ = REBASE_OPCODE_DONE;
  return p - out;
}

////////////////////////////////////////////////////////////////////////////////
// what the elf side has to say

static Elf64_Ehdr *elf_header;

static void parse_elf(void) {
  elf_header = (Elf64_Ehdr *)debug;
  if (debugsize < sizeof(Elf64_Ehdr) || memcmp(debug, ELFMAG, 4))
    die("the debug file isn't an elf");
}

static Elf64_Shdr *elf_section(int i) {
  return (Elf64_Shdr *)(debug + elf_header->e_shoff + i * elf_header->e_shentsize);
}

static uint64_t elf_symbol(const char *want) {
  for (int i = 0; i < elf_header->e_shnum; ++i) {
    Elf64_Shdr *sh = elf_section(i);
    if (sh->sh_type != SHT_SYMTAB)
      continue;
    Elf64_Sym *syms = (Elf64_Sym *)(debug + sh->sh_offset);
    const char *strs = debug + elf_section(sh->sh_link)->sh_offset;
    for (size_t j = 0; j < sh->sh_size / sizeof(Elf64_Sym); ++j)
      if (!strcmp(strs + syms[j].st_name, want))
        return syms[j].st_value;
  }
  return 0;
}

/**
 * Collects every word the linker resolved to an address in the image.
 *
 * Two sources, because a linker leaves two kinds behind. The relocation
 * records say where the ones it copied are, and the global offset table
 * holds the ones it made itself, which no record mentions.
 */
static void collect_rebases(void) {
  int is_arm = elf_header->e_machine == EM_AARCH64;
  for (int i = 0; i < elf_header->e_shnum; ++i) {
    Elf64_Shdr *sh = elf_section(i);
    if (sh->sh_type != SHT_RELA)
      continue;
    Elf64_Shdr *target = elf_section(sh->sh_info);
    if (!(target->sh_flags & SHF_ALLOC))
      continue;  // debug info describes the image, it isn't in it
    Elf64_Rela *rela = (Elf64_Rela *)(debug + sh->sh_offset);
    for (size_t j = 0; j < sh->sh_size / sizeof(Elf64_Rela); ++j) {
      uint32_t type = ELF64_R_TYPE(rela[j].r_info);
      if (type == (is_arm ? R_AARCH64_ABS64 : R_X86_64_64))
        want_rebase(rela[j].r_offset);
    }
  }
  uint64_t got = elf_symbol("__got_start"), got_end = elf_symbol("__got_end");
  if (!got || !got_end)
    die("the linker script stopped bracketing the global offset table");
  uint64_t low = segments[0].vmaddr, high = 0;
  for (int i = 0; i < nsegments; ++i)
    if (segments[i].vmaddr + segments[i].vmsize > high)
      high = segments[i].vmaddr + segments[i].vmsize;
  for (uint64_t at = got; at < got_end; at += POINTER_SIZE) {
    uint64_t *word = at_vaddr(at);
    if (word && *word >= low && *word < high)
      want_rebase(at);
  }
}

////////////////////////////////////////////////////////////////////////////////
// the bind opcodes

static size_t build_bind(unsigned char *out, size_t room) {
  uint64_t begin = elf_symbol("ape_macho_imports");
  uint64_t end = elf_symbol("ape_macho_imports_end");
  if (!begin || begin == end)
    return 0;  // a library that asks the host for nothing
  unsigned char *p = out;
  *p++ = BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER;
  uint64_t ordinal = ~0ull;
  for (uint64_t at = begin; at < end; at += APE_MACHO_IMPORT_STRIDE) {
    struct Import *im = at_vaddr(at);
    if (!im || !im->slot)
      continue;
    struct Segment *seg = segment_of(im->slot);
    if (!seg)
      die("%s wants an address put somewhere outside the image", im->name);
    if (im->flags != ordinal) {
      ordinal = im->flags;
      if (im->flags == APE_IMPORT_FLAT)
        // -2 means look in everything already loaded, which is how a
        // plugin reaches the program that loaded it
        *p++ = BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | (0x0f & -2);
      else
        *p++ = BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1;
    }
    *p++ = BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM;
    size_t len = strlen(im->name) + 1;
    memcpy(p, im->name, len);
    p += len;
    *p++ = BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg->index;
    p += put_uleb(p, im->slot - seg->vmaddr);
    *p++ = BIND_OPCODE_DO_BIND;
    printf("bind %s into %s+%#lx\n", im->name, seg->name,
           im->slot - seg->vmaddr);
    if ((size_t)(p - out) + APE_MACHO_IMPORT_STRIDE > room)
      die("the bind opcodes need more than the %zu bytes set aside; raise "
          "APE_MACHO_BIND_SIZE",
          room);
  }
  *p++ = BIND_OPCODE_DONE;
  return p - out;
}

int main(int argc, char *argv[]) {
  prog = argc > 0 ? basename(argv[0]) : "apedylib";
  if (argc != 3)
    die("usage: %s IMAGE DEBUG", prog);

  image = slurp(argv[1], &imagesize);
  debug = slurp(argv[2], &debugsize);
  parse_macho();
  parse_elf();

  size_t n;
  n = build_trie((unsigned char *)image + dyldinfo->export_off,
                 dyldinfo->export_size, segments[0].vmaddr);
  dyldinfo->export_size = n;

  n = build_bind((unsigned char *)image + dyldinfo->bind_off,
                 dyldinfo->bind_size);
  dyldinfo->bind_size = n;

  collect_rebases();
  n = build_rebase((unsigned char *)image + dyldinfo->rebase_off,
                   dyldinfo->rebase_size);
  dyldinfo->rebase_size = n;
  printf("rebase %zu addresses in %zu bytes\n", nrebases, n);

  // a segment claiming more of the file than the file has is a bus error
  // the moment dyld touches the end of it
  for (int i = 0; i < nsegments; ++i)
    if (segments[i].fileoff + segments[i].filesize > imagesize)
      die("%s runs to %#lx and the file stops at %#zx", segments[i].name,
          segments[i].fileoff + segments[i].filesize, imagesize);

  int fd = open(argv[1], O_WRONLY | O_TRUNC);
  if (fd == -1 || write(fd, image, imagesize) != (ssize_t)imagesize)
    die("%s: could not write", argv[1]);
  close(fd);
  return 0;
}
