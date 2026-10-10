/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — vitest: arranque y manejador de interrupciones.
 *
 * El IPL3 de libdragon carga los segmentos del ELF, deja $sp al final de la
 * RDRAM y salta a _start. Aquí se limpia .bss (no .fb) y se llama a main.
 *
 * exc_vector se copia a 0x80000180 (vector general). El manejador solo usa
 * k0/k1: si es la interrupción del VI, guarda VI_V_CURRENT y Count, la baja
 * escribiendo VI_V_CURRENT y cuenta el campo. Cualquier otra excepción se
 * queda en un bucle (con EXL activo: un emulador lo detecta como colgado).
 */
	.set noreorder
	.set noat
	.set mips3              /* eret */

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
	lui   $k0, %hi(exc_handler)
	addiu $k0, $k0, %lo(exc_handler)
	jr    $k0
	nop

exc_handler:
	mfc0  $k0, $13              /* Cause */
	nop
	andi  $k0, $k0, 0x7C        /* ExcCode */
	bnez  $k0, exc_fatal
	nop
	lui   $k0, 0xA440
	lw    $k1, 0x10($k0)        /* VI_V_CURRENT */
	sw    $zero, 0x10($k0)      /* baja la interrupción del VI */
	lui   $k0, 0xA430
	lw    $zero, 0x08($k0)      /* MI_INTR: espera a que la escritura llegue */
	lui   $k0, %hi(vi_vcurrent)
	sw    $k1, %lo(vi_vcurrent)($k0)
	mfc0  $k1, $9               /* Count */
	nop
	lui   $k0, %hi(vi_count)
	sw    $k1, %lo(vi_count)($k0)
	lui   $k0, %hi(vi_fields)
	lw    $k1, %lo(vi_fields)($k0)
	nop
	addiu $k1, $k1, 1
	sw    $k1, %lo(vi_fields)($k0)
	eret
	nop

exc_fatal:
	b     exc_fatal
	nop
