/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la máquina: CPU + bus + planificador.
 *
 * Toda la memoria va dentro de la estructura salvo la RDRAM, que la pone
 * quien llama. Una instancia ocupa ~10 KB más la RDRAM; en la PS3 se
 * reserva una sola vez al arrancar.
 */
#ifndef SALSA64_CORE_N64_H
#define SALSA64_CORE_N64_H

#include "core/bus.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

struct salsa64_n64 {
	struct salsa64_cpu cpu;
	struct salsa64_sched sched;
	struct salsa64_bus bus;
	/* Botón Reset (reset suave): PRE_NMI y, medio segundo después, NMI. */
	bool nmi_pending;
	uint64_t nmi_at;  /* ciclo del CPU en que toca el NMI */
};

/* Ciclos entre el PRE_NMI y el NMI: ~0,5 s (n64brew.dev, «PIF-NUS»: el PIF
 * da tiempo a que el juego guarde antes de reiniciar). */
#define SALSA64_NMI_DELAY_CYCLES 46875000ull

void salsa64_n64_init(struct salsa64_n64 *n64, uint8_t *rdram, uint32_t rdram_size);
/* Reset en frío de todo (RDRAM incluida). */
void salsa64_n64_reset(struct salsa64_n64 *n64);
/* Reset suave de la máquina (RDRAM intacta): lo del reset en frío salvo la
 * memoria. Quien llama carga después el arranque (salsa64_boot_nmi). */
void salsa64_n64_reset_warm(struct salsa64_n64 *n64);
/* Botón Reset: levanta la interrupción de PRE_NMI (IP4 del CPU, la que
 * libultra atiende para guardar y callar el audio) y fija el NMI medio
 * segundo después. Una segunda pulsación con la primera en curso no hace
 * nada. */
void salsa64_n64_prenmi(struct salsa64_n64 *n64);
/* ¿Toca ya el NMI? (se mira al final de cada campo) */
bool salsa64_n64_nmi_due(const struct salsa64_n64 *n64);
/* Ejecuta `cycles` ciclos del VR4300 (93,75 MHz). */
void salsa64_n64_run(struct salsa64_n64 *n64, uint64_t cycles);
/* Ejecuta hasta que termina el campo de video en curso (fin de cuadro del
 * VI, ~1,57 M ciclos en NTSC) y devuelve los ciclos ejecutados. Al volver,
 * salsa64_vi_frame describe la imagen de ese campo. */
uint64_t salsa64_n64_run_field(struct salsa64_n64 *n64);
/* Pide terminar salsa64_n64_run después de la instrucción en curso (por
 * ejemplo, desde el callback del ISViewer). */
void salsa64_n64_stop(struct salsa64_n64 *n64);

#endif
