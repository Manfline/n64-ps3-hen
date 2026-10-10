/*
 * Retro64 — Audio Interface (AI).
 *
 * Primer hito: solo guarda los registros; escribir AI_STATUS baja la
 * interrupción del AI. AI_LEN y AI_STATUS leen 0 (FIFO vacía, sin DMA en
 * curso). Falta el DMA de audio, la FIFO de dos entradas y la salida (SPE 1).
 */
#include "core/bus.h"

uint32_t retro64_ai_read(struct retro64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= RETRO64_AI_NREGS || i == RETRO64_AI_LEN || i == RETRO64_AI_STATUS)
		return 0;
	return bus->ai[i];
}

void retro64_ai_write(struct retro64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	if (i >= RETRO64_AI_NREGS)
		return;
	if (i == RETRO64_AI_STATUS) {
		retro64_mi_clear(bus, RETRO64_IRQ_AI);
		return;
	}
	bus->ai[i] = v;
}
