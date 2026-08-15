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
#define APE_MACHO_REBASE_SIZE 16384

/**
 * How much of __LINKEDIT is set aside for mach-o bind opcodes.
 *
 * These say which words dyld should fill with addresses from somewhere
 * else, which is the only way a library can reach its host.
 */
#define APE_MACHO_BIND_SIZE 512

/**
 * How much of __LINKEDIT is set aside for the mach-o export trie.
 *
 * Built after the link, out of the symbol table, since the offsets
 * inside it are uleb128 and depend on what the whole thing came to.
 */
#define APE_MACHO_TRIE_SIZE 4096

/**
 * How much room each .import directive takes.
 *
 * Fixed width so the table can be walked without having read it.
 */
#define APE_MACHO_IMPORT_STRIDE 80

/**
 * Where an imported symbol is looked for.
 *
 * A named library, meaning the first one this image loads, or whatever
 * is already in the process, which is how a plugin reaches the program
 * that opened it.
 */
#define APE_IMPORT_FROM_DYLIB 0
#define APE_IMPORT_FLAT 1

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

/**
 * How wide each mach-o string table entry is.
 *
 * Fixed, so which string belongs to a symbol falls out of where the
 * symbol is, which is what lets exports be written in more than one
 * file. Sixty four bytes fits any reasonable name with the leading
 * underscore mach-o expects.
 */
#define MACHO_STRTAB_STRIDE 64

#ifdef __ASSEMBLER__
#include "libc/dce.h"

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
ape_export_index = 0

.macro	.export	symbol:req
#if SupportsWindows()
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
#endif /* SupportsWindows() */
//	The mach-o side wants the same thing shaped differently: an nlist
//	pointing into a string table by byte offset. That offset isn't
//	something the assembler can work out across sections, so the
//	strings are fixed width and the offset falls out of the index.
#ifdef __aarch64__
//	Everything a host reaches has to go through one of these. The
//	thread pointer lives in a register the host is entitled to be using
//	for its own purposes, so what gets published is a thunk that swaps
//	it, not the function itself.
 .section .text.ape.export.\symbol,"ax",@progbits
	.balign	4
.Lape.export.\symbol:
	stp	x29,x30,[sp,#-32]!
	mov	x29,sp
	str	x28,[sp,#16]
	bl	__ape_load_tls
	bl	\symbol
	ldr	x28,[sp,#16]
	ldp	x29,x30,[sp],#32
	ret
 .previous
#define MACHO_EXPORT_ADDRESS(SYMBOL) .Lape.export.\symbol
#else
#define MACHO_EXPORT_ADDRESS(SYMBOL) \symbol
#endif
 .section .macho.linkedit.1.syms.1.\symbol,"a",@progbits
	.long	1 + ape_export_index * MACHO_STRTAB_STRIDE	// n_strx
	.byte	0x0f			// n_type: N_SECT|N_EXT
	.byte	1			// n_sect: __text
	.short	0			// n_desc
	.quad	MACHO_EXPORT_ADDRESS(\symbol)	// n_value
 .previous
 .section .macho.linkedit.2.strs.1.\symbol,"a",@progbits
	.asciz	"_\symbol"
	.org	MACHO_STRTAB_STRIDE,0	// pad this fragment to the stride
 .previous
 ape_export_index = ape_export_index + 1
.endm

//	Says that a word in this image is to be filled in with the address
//	of something outside it, which is the only way a library reaches
//	whatever loaded it.
//
//	The table is walked after the link, so unlike the export directory
//	these can be written wherever they belong.
.macro	.import	slot:req symbol:req where=APE_IMPORT_FROM_DYLIB
 .section .macho.imports.1.\symbol,"a",@progbits
	.quad	\slot
	.quad	\where
	.asciz	"\symbol"
	.org	APE_MACHO_IMPORT_STRIDE,0
 .previous
.endm
#endif /* __ASSEMBLER__ */

#endif /* COSMOPOLITAN_APE_RELOCATIONS_H_ */
