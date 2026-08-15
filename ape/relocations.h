#ifndef COSMOPOLITAN_APE_RELOCATIONS_H_
#define COSMOPOLITAN_APE_RELOCATIONS_H_
/*─────────────────────────────────────────────────────────────────────────────╗
│ αcτµαlly pδrταblε εxεcµταblε § relocations                                   │
╚──────────────────────────────────────────────────────────────────────────────╝
  One of the things αcτµαlly pδrταblε εxεcµταblε does a good job
  abstracting, is how a program needs to exist at three addresses
  simultaneously during the early stages of the loading process.

  By default, the linker calculates all symbols using virtual addresses.
  In some cases it's necessary to use addend macros that change virtual
  addresses into the other two types: physical and real. */

#define IMAGE_BASE_REAL 0x2000

#ifndef IMAGE_BASE_VIRTUAL
#define IMAGE_BASE_VIRTUAL 0x4200000
#endif

#ifndef IMAGE_BASE_PHYSICAL
#define IMAGE_BASE_PHYSICAL 0x100000
#endif

/**
 * Returns Relative Virtual Address.
 */
#define RVA(x) ((x) - (IMAGE_BASE_VIRTUAL))

/**
 * How much of __LINKEDIT is set aside for mach-o rebase opcodes.
 *
 * They can't be written until the link is done, so the space is reserved
 * and filled afterwards. Generous: what goes unused stays zero, which is
 * the opcode that says stop.
 */
#define APE_MACHO_REBASE_SIZE 65536

/**
 * Adjusts virtual address so it's relative to load address.
 */
#define PHYSICAL(x) ((x) - (IMAGE_BASE_VIRTUAL - IMAGE_BASE_PHYSICAL))

/**
 * Makes high-entropy read-only addresses relocatable in real mode.
 */
#define REAL(x) ((x) - (IMAGE_BASE_VIRTUAL - IMAGE_BASE_REAL))

#if IMAGE_BASE_VIRTUAL % 0x1000 != 0
#error "IMAGE_BASE_VIRTUAL must be 4kb aligned"
#endif
#if IMAGE_BASE_PHYSICAL % 0x1000 != 0
#error "IMAGE_BASE_PHYSICAL must be 4kb aligned"
#endif
#if IMAGE_BASE_REAL % 0x1000 != 0
#error "IMAGE_BASE_REAL must be 4kb aligned"
#endif

#ifdef __ASSEMBLER__
//	Publishes a symbol in the PE export directory, so The New
//	Technology can find it by name when the image is a library.
//
//	The three parallel arrays are built from decentralized fragments
//	the linker sorts by name. Since the function and name arrays sort
//	on the same key they stay aligned, so each ordinal is just the
//	index of its own slot.
//
//	The assembler can't take the difference of two symbols in
//	different sections, so the index comes from a counter here, which
//	means .export directives have to be listed in the order the linker
//	will sort them: alphabetically. A generated export list gets that
//	for free.
//
//	@see	ape/ape.S for the directory this feeds
//	Mach-O string table entries are fixed width so their offsets can be
//	computed from the index. Sixty four bytes fits any reasonable
//	exported name with the leading underscore mach-o expects.
#define MACHO_STRTAB_STRIDE 64
#define MACHO_TRIE_ROOT_SIZE 2
#define MACHO_TRIE_NODE_SIZE 8
ape_export_index = 0

//	Where an export's trie node lands, measured from the root. The
//	edges are variable length and the nodes aren't, so this only needs
//	the total size of the former, which a label at the end gives.
#define MACHO_TRIE_OFFSET(SYMBOL)                        \
  (MACHO_TRIE_ROOT_SIZE +                                \
   (.Lmacho_trie_edges_end - .Lmacho_trie_edges) +       \
   ape_export_index * MACHO_TRIE_NODE_SIZE)
.macro	.export	symbol:req
 .section .sort.rodata.pe.edata.2.1.\symbol,"a",@progbits
.Lpe.func.\symbol:
	.long	RVA(\symbol)
 .previous
 .section .sort.rodata.pe.edata.3.1.\symbol,"a",@progbits
	.long	RVA(.Lpe.name.\symbol)
 .previous
 .section .sort.rodata.pe.edata.4.1.\symbol,"a",@progbits
	.short	ape_export_index
 .previous
 .section .sort.rodata.pe.edata.6.1.\symbol,"a",@progbits
.Lpe.name.\symbol:
	.asciz	"\symbol"
 .previous
//	The mach-o side wants the same thing shaped differently: an nlist
//	pointing into a string table by byte offset. That offset isn't
//	something the assembler can work out across sections, so the
//	strings are fixed width and the offset falls out of the index.
 .section .macho.linkedit.1.syms.1.\symbol,"a",@progbits
	.long	1 + ape_export_index * MACHO_STRTAB_STRIDE	// n_strx
	.byte	0x0f			// n_type: N_SECT|N_EXT
	.byte	1			// n_sect: __text
	.short	0			// n_desc
	.quad	\symbol			// n_value
 .previous
 .section .macho.linkedit.2.strs.1.\symbol,"a",@progbits
	.asciz	"_\symbol"
	.org	MACHO_STRTAB_STRIDE,0	// pad this fragment to the stride
 .previous
//	dyld looks up a symbol by walking a trie, which is what dlsym()
//	reads; the symbol table above is only there to be listed. The root
//	node belongs to the linker script, since only it knows how many
//	exports there were. What each export contributes is one edge out
//	of the root and the node that edge leads to.
//
//	Offsets within a trie are uleb128, which no relocation can encode,
//	so both are arranged to be arithmetic the assembler can do itself:
//	every node is the same size, and the edges are measured with a
//	label. That means all of a program's .export directives have to sit
//	in one translation unit, which is how they get written anyway. The
//	address is the one thing left over, being a link time value, so it
//	is written into the image afterwards.
 .section .macho.trie.1.edges,"a",@progbits
 .if ape_export_index == 0
.Lmacho_trie_edges:
 .endif
	.asciz	"_\symbol"
	.byte	MACHO_TRIE_OFFSET(\symbol) & 0x7f | 0x80
	.byte	(MACHO_TRIE_OFFSET(\symbol) >> 7) & 0x7f
 .previous
 .section .macho.trie.2.nodes,"a",@progbits
	.byte	6			// terminal size: the two below
	.byte	0			// flags: a regular export
	.byte	0x80,0x80,0x80,0x80,0	// address, written in after linking
	.byte	0			// number of children
 .previous
 ape_export_index = ape_export_index + 1
.endm

//	Closes the export trie. Goes after the last .export, and only in
//	the file they're written in.
.macro	.exports_end
 .section .macho.trie.1.edges,"a",@progbits
.Lmacho_trie_edges_end:
 .previous
.endm
#endif /* __ASSEMBLER__ */

#endif /* COSMOPOLITAN_APE_RELOCATIONS_H_ */
