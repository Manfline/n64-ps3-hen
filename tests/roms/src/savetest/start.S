/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — savetest: arranque.
 *
 * El IPL3 de libdragon carga los segmentos del ELF, deja $sp al final de la
 * RDRAM y salta a _start. Aquí se limpia .bss (no .fb) y se llama a main.
 *
 * exc_vector se copia a 0x80000180 (vector general): la ROM no usa
 * interrupciones, así que cualquier excepción se queda ahí con EXL activo
 * (un emulador lo detecta como colgado).
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

	.text
	.globl exc_vector
	.align 4
exc_vector:
	b     exc_vector
	nop
