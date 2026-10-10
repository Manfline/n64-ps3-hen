/*
 * Salsa64 — Audio Interface (AI).
 *
 * Primer hito: solo guarda los registros; escribir AI_STATUS baja la
 * interrupción del AI. AI_LEN y AI_STATUS leen 0 (FIFO vacía, sin DMA en
 * curso). Falta el DMA de audio, la FIFO de dos entradas y la salida (SPE 1).
 */
#include "core/bus.h"

uint32_t salsa64_ai_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_AI_NREGS || i == SALSA64_AI_LEN || i == SALSA64_AI_STATUS)
		return 0;
	return bus->ai[i];
}

void salsa64_ai_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_AI_NREGS)
		return;
	if (i == SALSA64_AI_STATUS) {
		salsa64_mi_clear(bus, SALSA64_IRQ_AI);
		return;
	}
	bus->ai[i] = v;
}
