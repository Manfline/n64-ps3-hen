/*
 * Salsa64 — Video Interface (VI).
 *
 * Primer hito: solo guarda los registros; escribir VI_V_CURRENT baja la
 * interrupción del VI. Falta el contador de líneas, la interrupción en
 * VI_V_INTR (evento del planificador) y la salida del framebuffer.
 */
#include "core/bus.h"

uint32_t salsa64_vi_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	return i < SALSA64_VI_NREGS ? bus->vi[i] : 0;
}

void salsa64_vi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_VI_NREGS)
		return;
	if (i == SALSA64_VI_V_CURRENT) {
		salsa64_mi_clear(bus, SALSA64_IRQ_VI);
		return;
	}
	bus->vi[i] = v;
}
