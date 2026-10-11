/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — MIPS Interface (MI): modo, versión e interrupciones del RCP.
 * La salida (MI_INTR & MI_INTR_MASK) es la línea IP2 del VR4300.
 */
#include <string.h>

#include "core/bus.h"
#include "cpu/vr4300.h"

enum { MI_MODE = 0x00, MI_VERSION = 0x04, MI_INTR = 0x08, MI_INTR_MASK = 0x0C };

static void mi_update(struct salsa64_bus *bus)
{
	if (bus->cpu)
		salsa64_cpu_set_ip(bus->cpu, 2, (bus->mi.intr & bus->mi.mask) != 0);
}

void salsa64_mi_raise(struct salsa64_bus *bus, enum salsa64_irq irq)
{
	bus->mi.intr |= 1u << irq;
	mi_update(bus);
}

void salsa64_mi_clear(struct salsa64_bus *bus, enum salsa64_irq irq)
{
	bus->mi.intr &= ~(1u << irq);
	mi_update(bus);
}

void salsa64_rcp_reset(struct salsa64_bus *bus)
{
	memset(&bus->mi, 0, sizeof bus->mi);
	memset(&bus->sp, 0, sizeof bus->sp);
	memset(bus->dp, 0, sizeof bus->dp);
	memset(bus->vi.regs, 0, sizeof bus->vi.regs);
	bus->vi.regs[SALSA64_VI_V_INTR] = SALSA64_VI_V_INTR_RESET;
	salsa64_ai_reset(bus);
	memset(bus->pi, 0, sizeof bus->pi);
	memset(bus->si, 0, sizeof bus->si);
	bus->sp.regs[SALSA64_SP_STATUS] = SALSA64_SP_STATUS_HALT;
	salsa64_rsp_reset(bus);
	mi_update(bus);
}

uint32_t salsa64_mi_read(struct salsa64_bus *bus, uint32_t reg)
{
	switch (reg) {
	case MI_MODE:      return bus->mi.mode;
	case MI_VERSION:   return SALSA64_MI_VERSION_VALUE;
	case MI_INTR:      return bus->mi.intr;
	case MI_INTR_MASK: return bus->mi.mask;
	default:           return 0;
	}
}

void salsa64_mi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	switch (reg) {
	case MI_MODE: {
		uint32_t m = (bus->mi.mode & ~0x7Fu) | (v & 0x7F);  /* largo de init */
		if (v & 0x0080) m &= ~0x080u;  /* init mode */
		if (v & 0x0100) m |= 0x080;
		if (v & 0x0200) m &= ~0x100u;  /* ebus test */
		if (v & 0x0400) m |= 0x100;
		if (v & 0x1000) m &= ~0x200u;  /* RDRAM reg mode */
		if (v & 0x2000) m |= 0x200;
		bus->mi.mode = m;
		if (v & 0x0800)
			salsa64_mi_clear(bus, SALSA64_IRQ_DP);
		return;
	}
	case MI_INTR_MASK:
		/* Pares de bits (limpiar, poner) para SP, SI, AI, VI, PI, DP. */
		for (int i = 0; i < 6; i++) {
			if (v & (1u << (2 * i)))
				bus->mi.mask &= ~(1u << i);
			if (v & (2u << (2 * i)))
				bus->mi.mask |= 1u << i;
		}
		mi_update(bus);
		return;
	default:
		return;
	}
}
