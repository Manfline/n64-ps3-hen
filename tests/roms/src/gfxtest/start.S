/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — gfxtest: arranque. El IPL3 de libdragon carga los segmentos
 * del ELF, deja $sp al final de la RDRAM y salta a _start. Aquí se limpia
 * .bss (no .fb ni .rsp) y se llama a main. Sin interrupciones: la ROM
 * consulta MI_INTR.
 */
	.set noreorder
	.set noat

	.section .text.start, "ax"
	.globl _start
_start:
	lui   $t0, %hi(__bss_start)
	addiu $t0, $t0, %lo(__bss_start)
	lui   $t1, %hi(__bss_end)
	addiu $t1, $t1, %lo(__bss_end)
1:	beq   $t0, $t1, 2f
	nop
	sw    $zero, 0($t0)
	b     1b
	addiu $t0, $t0, 4
2:	jal   main
	nop
3:	b     3b
	nop
