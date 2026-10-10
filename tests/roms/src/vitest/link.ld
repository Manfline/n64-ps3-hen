/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — vitest: un solo segmento cargable desde 0x80000400. */
ENTRY(_start)

PHDRS
{
	load PT_LOAD;
}

SECTIONS
{
	. = 0x80000400;
	.text : { KEEP(*(.text.start)) *(.text .text.*) } :load
	.rodata : { *(.rodata .rodata.*) } :load
	.data : { *(.data .data.*) *(.sdata .sdata.*) } :load
	.bss (NOLOAD) : {
		. = ALIGN(4);
		__bss_start = .;
		*(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} :load
	.fb (NOLOAD) : { . = ALIGN(64); *(.fb) } :load
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
