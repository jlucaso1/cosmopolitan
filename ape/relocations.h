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
ape_export_index = 0
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
 ape_export_index = ape_export_index + 1
 .section .sort.rodata.pe.edata.6.1.\symbol,"a",@progbits
.Lpe.name.\symbol:
	.asciz	"\symbol"
 .previous
.endm
#endif /* __ASSEMBLER__ */

#endif /* COSMOPOLITAN_APE_RELOCATIONS_H_ */
