/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — aviso en el log de los fallos del CPU del juego. Ver faultlog.h.
 */
#include "core/faultlog.h"

#include "core/bus.h"

#define SEEN_MAX 16

static salsa64_faultlog_fn log_fn;
static struct {
	uint32_t pc;
	uint8_t code;
} seen[SEEN_MAX];
static unsigned nseen;

static const char *exc_name(unsigned code)
{
	switch (code) {
	case SALSA64_EXC_ADEL: return "AdEL (dirección inválida al leer o buscar una instrucción)";
	case SALSA64_EXC_ADES: return "AdES (dirección inválida al escribir)";
	case SALSA64_EXC_IBE:  return "IBE (error de bus al buscar una instrucción)";
	case SALSA64_EXC_DBE:  return "DBE (error de bus en un dato)";
	default:               return NULL;
	}
}

static void on_exception(struct salsa64_cpu *cpu)
{
	unsigned code = SALSA64_CAUSE_EXC(cpu->cause);
	const char *name = exc_name(code);
	if (!name || !log_fn)
		return;
	/* El pc de la instrucción que falló, en KSEG0 (extensión de signo de
	 * 0x80000000–0x9FFFFFFF): el código del juego. Un salto a una dirección
	 * mala (un callback pisado) falla al buscar la instrucción: EPC es la
	 * dirección mala (no está en KSEG0) y quien llamó se ve en ra. */
	uint64_t pc = cpu->epc, ra = cpu->gpr[31];
	bool game = pc >= 0xFFFFFFFF80000000ull && pc < 0xFFFFFFFFA0000000ull;
	bool jump = pc == cpu->bad_vaddr && ra >= 0xFFFFFFFF80000000ull && ra < 0xFFFFFFFFA0000000ull;
	if (!game && !jump)
		return;
	for (unsigned i = 0; i < nseen; i++)
		if (seen[i].code == code && seen[i].pc == (uint32_t)pc)
			return;
	if (nseen < SEEN_MAX) {
		seen[nseen].code = (uint8_t)code;
		seen[nseen].pc = (uint32_t)pc;
		nseen++;
	}
	log_fn("cpu: fallo %s en el campo %u: pc 0x%08X, dirección 0x%016llX, ra 0x%08X, sp 0x%08X", name,
	       cpu->bus ? cpu->bus->vi.fields : 0, (unsigned)pc, (unsigned long long)cpu->bad_vaddr,
	       (unsigned)ra, (unsigned)cpu->gpr[29]);
}

void salsa64_faultlog_install(struct salsa64_cpu *cpu, salsa64_faultlog_fn log)
{
	log_fn = log;
	nseen = 0;
	if (!cpu->exc_hook)
		cpu->exc_hook = on_exception;
}
