/*
 * Retro64 — la máquina: CPU + bus + planificador.
 *
 * Toda la memoria va dentro de la estructura salvo la RDRAM, que la pone
 * quien llama. Una instancia ocupa ~10 KB más la RDRAM; en la PS3 se
 * reserva una sola vez al arrancar.
 */
#ifndef RETRO64_CORE_N64_H
#define RETRO64_CORE_N64_H

#include "core/bus.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

struct retro64_n64 {
	struct retro64_cpu cpu;
	struct retro64_sched sched;
	struct retro64_bus bus;
};

void retro64_n64_init(struct retro64_n64 *n64, uint8_t *rdram, uint32_t rdram_size);
/* Reset en frío de todo (RDRAM incluida). */
void retro64_n64_reset(struct retro64_n64 *n64);
/* Ejecuta `cycles` ciclos del VR4300 (93,75 MHz). */
void retro64_n64_run(struct retro64_n64 *n64, uint64_t cycles);

#endif
