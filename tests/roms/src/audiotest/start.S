/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — audiotest: arranque y manejador de interrupciones.
 *
 * El IPL3 de libdragon carga los segmentos del ELF, deja $sp al final de la
 * RDRAM y salta a _start. Aquí se limpia .bss (no .fb) y se llama a main.
 *
 * exc_vector se copia a 0x80000180 (vector general). El manejador guarda
 * t0–t2 en exc_save y atiende, según MI_INTR:
 *   AI  la baja escribiendo AI_STATUS, cuenta la interrupción y guarda
 *       Count en ai_counts[ai_irqs & 63]
 *   VI  la baja escribiendo VI_V_CURRENT y cuenta el campo
 * Cualquier otra excepción se queda en un bucle (con EXL activo: un
 * emulador lo detecta como colgado).
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
	lui   $k0, %hi(exc_save)
	addiu $k0, $k0, %lo(exc_save)
	sw    $t0, 0($k0)
	sw    $t1, 4($k0)
	sw    $t2, 8($k0)
	mfc0  $t0, $13              /* Cause */
	nop
	andi  $t0, $t0, 0x7C        /* ExcCode */
	bnez  $t0, exc_fatal
	nop
	lui   $t0, 0xA430
	lw    $t1, 0x08($t0)        /* MI_INTR */
	nop
	andi  $t2, $t1, 0x04        /* AI */
	beqz  $t2, 1f
	nop
	lui   $t0, 0xA450
	sw    $zero, 0x0C($t0)      /* AI_STATUS: baja la interrupción */
	lui   $t0, %hi(ai_irqs)
	lw    $t2, %lo(ai_irqs)($t0)
	nop
	andi  $t2, $t2, 63
	sll   $t2, $t2, 2
	lui   $t0, %hi(ai_counts)
	addiu $t0, $t0, %lo(ai_counts)
	addu  $t0, $t0, $t2
	mfc0  $t2, $9               /* Count */
	nop
	sw    $t2, 0($t0)
	lui   $t0, %hi(ai_irqs)
	lw    $t2, %lo(ai_irqs)($t0)
	nop
	addiu $t2, $t2, 1
	sw    $t2, %lo(ai_irqs)($t0)
1:	andi  $t2, $t1, 0x08        /* VI */
	beqz  $t2, 2f
	nop
	lui   $t0, 0xA440
	sw    $zero, 0x10($t0)      /* VI_V_CURRENT: baja la interrupción */
	lui   $t0, %hi(vi_fields)
	lw    $t2, %lo(vi_fields)($t0)
	nop
	addiu $t2, $t2, 1
	sw    $t2, %lo(vi_fields)($t0)
2:	lui   $t0, 0xA430
	lw    $zero, 0x08($t0)      /* MI_INTR: espera a que las escrituras lleguen */
	lw    $t0, 0($k0)
	lw    $t1, 4($k0)
	lw    $t2, 8($k0)
	eret
	nop

exc_fatal:
	b     exc_fatal
	nop
