/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — detección de bucles de espera del VR4300.
 *
 * Los juegos y los homebrew esperan al VI (o a cualquier interrupción) en
 * bucles cortos: el `for (;;);` del hilo ocioso de libultra (`b .` con NOP)
 * o un sondeo de una variable que cambia el manejador de interrupciones:
 *
 *     espera: lw   $1, campos($19)
 *             beq  $1, $3, espera
 *             nop
 *
 * Mientras no llegue un evento del planificador, cada vuelta deja la
 * máquina exactamente igual. Interpretarlas es tiempo perdido: casi todo el
 * campo en escenas ligeras.
 *
 * Criterio (exacto, sin heurísticas de tiempo): cuando el intérprete toma un
 * salto corto hacia atrás (interp.c, idle_hint), aquí se simula una vuelta
 * completa desde el estado actual (delay slot, cuerpo y el salto), en una
 * copia de los registros y sin tocar nada. Es un bucle de espera si:
 *   - solo hay operaciones de enteros sin efectos y cargas de la RDRAM en
 *     modo kernel, por KSEG0/KSEG1 o por la TLB (nada de almacenamientos,
 *     E/S, COP0, FPU ni saltos intermedios),
 *   - el salto se vuelve a tomar al mismo destino, y
 *   - los 32 registros terminan iguales a como empezaron.
 * Entonces la vuelta es un punto fijo: sin un evento nada cambia (la RDRAM
 * solo la escribe el CPU, y el DMA ocurre al escribir los registros del
 * RCP). Se suman vueltas enteras hasta el próximo evento, así que la
 * interrupción cae en el mismo ciclo y en la misma instrucción del bucle
 * que sin el salto: Count, EPC y Random quedan idénticos.
 *
 * Condición para el resto del emulador: quien escriba la RDRAM fuera del
 * CPU (DMA con latencia, el RSP en un SPE) debe hacerlo en un evento del
 * planificador, o el salto se pasaría de largo esa escritura.
 *
 * Código y datos mapeados por la TLB (GoldenEye, Perfect Dark): se
 * traducen con la misma búsqueda que el intérprete pero sin efectos
 * (salsa64_vr4300_fetch_probe: sin excepciones ni cambios de la TLB). La
 * TLB no cambia dentro de la vuelta (COP0 descarta el bucle), y el «sí»
 * nunca se guarda: cada vez que se toma el salto se vuelve a simular con la
 * TLB y la memoria de ese momento, así que un remapeo o código reescrito
 * se ven en el siguiente análisis. Solo se guardan los descartes, por
 * pc ^ instrucción como en KSEG0: con el código remapeado bajo el mismo
 * salto, a lo sumo se deja de saltar, nunca se salta de más.
 *
 * Costo en el bucle caliente: solo los saltos tomados hacia atrás y cortos
 * consultan una tabla (pc ^ instrucción de los ya descartados). Un bucle que
 * no puede ser de espera (almacena, lee E/S…) se descarta al primer
 * intento; uno que falla el punto fijo (un contador), al segundo: el
 * primero puede fallar solo porque el delay slot aún no escribió su valor.
 *
 * Escrito desde la documentación del VR4300; la idea de saltar el bucle
 * ocioso es común a los emuladores de N64, el criterio de punto fijo es
 * propio.
 */
#include <string.h>

#include "core/sched.h"
#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

enum verdict {
	IDLE_LOOP,  /* punto fijo: se puede saltar */
	NOT_YET,    /* esta vez no (contador, delay slot, salida del bucle) */
	NEVER,      /* el código no puede ser un bucle de espera */
};

enum step {
	STEP_OK,
	STEP_BRANCH,  /* salto evaluado: `taken` y `target` */
	STEP_NOT_YET,
	STEP_NEVER,
};

/* La traducción sin efectos en modo kernel, alineada y dentro de la RDRAM:
 * KSEG0/KSEG1 restando la base (el camino rápido del intérprete) y lo demás
 * por la TLB, sin excepciones (si el intérprete lanzaría una, no es un
 * bucle de espera). */
static bool translate(const struct salsa64_cpu *cpu, uint64_t va, uint32_t size, uint32_t *pa)
{
	if ((va & (size - 1)) || cpu->mode != SALSA64_MODE_KERNEL)
		return false;
	uint32_t p;
	if ((va >> 30) == 0x3FFFFFFFEull)
		p = (uint32_t)va & 0x1FFFFFFF;
	else if (!salsa64_vr4300_fetch_probe(cpu, va, &p))
		return false;
	if (p >= cpu->rdram_size || cpu->rdram_size - p < size)
		return false;
	*pa = p;
	return true;
}

static bool fetch(const struct salsa64_cpu *cpu, uint64_t va, uint32_t *instr)
{
	uint32_t pa;
	if (!translate(cpu, va, 4, &pa))
		return false;
	*instr = salsa64_rd32(cpu->rdram + pa);
	return true;
}

static uint64_t load(const struct salsa64_cpu *cpu, uint32_t pa, uint32_t size, bool sign)
{
	const uint8_t *p = cpu->rdram + pa;
	switch (size) {
	case 1:  return sign ? salsa64_sx8(salsa64_rd8(p)) : salsa64_rd8(p);
	case 2:  return sign ? salsa64_sx16(salsa64_rd16(p)) : salsa64_rd16(p);
	case 4:  return sign ? salsa64_sx32(salsa64_rd32(p)) : salsa64_rd32(p);
	default: return salsa64_rd64(p);
	}
}

/* Simula la instrucción `i` en `pc` sobre los registros `g`. Las que
 * podrían lanzar una excepción por sus valores (desbordamiento) devuelven
 * STEP_NOT_YET: que la ejecute el intérprete. */
static enum step simulate(const struct salsa64_cpu *cpu, uint64_t *g, uint32_t i, uint64_t pc,
			  bool *taken, uint64_t *target)
{
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i), sa = I_SA(i), dst;
	uint64_t s = g[rs], t = g[rt], v, imm = I_SIMM(i);
	uint32_t size = 0, pa;
	bool sign = true;

	switch (I_OP(i)) {
	case 0x00:
		dst = rd;
		switch (I_FUNCT(i)) {
		case 0x00: v = salsa64_sx32((uint32_t)t << sa); break;                       /* SLL, NOP */
		case 0x02: v = salsa64_sx32((uint32_t)t >> sa); break;                       /* SRL */
		case 0x03: v = salsa64_sx32((uint32_t)((int64_t)t >> sa)); break;            /* SRA */
		case 0x04: v = salsa64_sx32((uint32_t)t << (s & 31)); break;                 /* SLLV */
		case 0x06: v = salsa64_sx32((uint32_t)t >> (s & 31)); break;                 /* SRLV */
		case 0x07: v = salsa64_sx32((uint32_t)((int64_t)t >> (s & 31))); break;      /* SRAV */
		case 0x0F: return STEP_OK;                                                   /* SYNC */
		case 0x10: v = cpu->hi; break;                                               /* MFHI */
		case 0x12: v = cpu->lo; break;                                               /* MFLO */
		case 0x20: {                                                                 /* ADD */
			uint32_t a = (uint32_t)s, b = (uint32_t)t, r = a + b;
			if ((~(a ^ b) & (a ^ r)) >> 31)
				return STEP_NOT_YET;
			v = salsa64_sx32(r);
			break;
		}
		case 0x21: v = salsa64_sx32((uint32_t)s + (uint32_t)t); break;               /* ADDU */
		case 0x22: {                                                                 /* SUB */
			uint32_t a = (uint32_t)s, b = (uint32_t)t, r = a - b;
			if (((a ^ b) & (a ^ r)) >> 31)
				return STEP_NOT_YET;
			v = salsa64_sx32(r);
			break;
		}
		case 0x23: v = salsa64_sx32((uint32_t)s - (uint32_t)t); break;               /* SUBU */
		case 0x24: v = s & t; break;                                                 /* AND */
		case 0x25: v = s | t; break;                                                 /* OR */
		case 0x26: v = s ^ t; break;                                                 /* XOR */
		case 0x27: v = ~(s | t); break;                                              /* NOR */
		case 0x2A: v = (int64_t)s < (int64_t)t; break;                               /* SLT */
		case 0x2B: v = s < t; break;                                                 /* SLTU */
		default:
			if (!cpu->allow64)
				return STEP_NEVER;
			switch (I_FUNCT(i)) {
			case 0x14: v = t << (s & 63); break;                                 /* DSLLV */
			case 0x16: v = t >> (s & 63); break;                                 /* DSRLV */
			case 0x17: v = (uint64_t)((int64_t)t >> (s & 63)); break;            /* DSRAV */
			case 0x2D: v = s + t; break;                                         /* DADDU */
			case 0x2F: v = s - t; break;                                         /* DSUBU */
			case 0x38: v = t << sa; break;                                       /* DSLL */
			case 0x3A: v = t >> sa; break;                                       /* DSRL */
			case 0x3B: v = (uint64_t)((int64_t)t >> sa); break;                  /* DSRA */
			case 0x3C: v = t << (sa + 32); break;                                /* DSLL32 */
			case 0x3E: v = t >> (sa + 32); break;                                /* DSRL32 */
			case 0x3F: v = (uint64_t)((int64_t)t >> (sa + 32)); break;           /* DSRA32 */
			default:   return STEP_NEVER;
			}
		}
		break;
	case 0x01:                                                                           /* REGIMM */
		if (rt > 0x03)  /* solo BLTZ, BGEZ, BLTZL, BGEZL */
			return STEP_NEVER;
		*taken = (rt & 1) ? (int64_t)s >= 0 : (int64_t)s < 0;
		*target = pc + 4 + (imm << 2);
		return STEP_BRANCH;
	case 0x02:                                                                           /* J */
		*taken = true;
		*target = ((pc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		return STEP_BRANCH;
	case 0x04: case 0x14: *taken = s == t; goto branch;                                  /* BEQ(L) */
	case 0x05: case 0x15: *taken = s != t; goto branch;                                  /* BNE(L) */
	case 0x06: case 0x16: *taken = (int64_t)s <= 0; goto branch;                         /* BLEZ(L) */
	case 0x07: case 0x17: *taken = (int64_t)s > 0; goto branch;                          /* BGTZ(L) */
	case 0x08: {                                                                         /* ADDI */
		uint32_t a = (uint32_t)s, b = (uint32_t)imm, r = a + b;
		if ((~(a ^ b) & (a ^ r)) >> 31)
			return STEP_NOT_YET;
		dst = rt;
		v = salsa64_sx32(r);
		break;
	}
	case 0x09: dst = rt; v = salsa64_sx32((uint32_t)s + (uint32_t)imm); break;          /* ADDIU */
	case 0x0A: dst = rt; v = (int64_t)s < (int64_t)imm; break;                          /* SLTI */
	case 0x0B: dst = rt; v = s < imm; break;                                            /* SLTIU */
	case 0x0C: dst = rt; v = s & I_UIMM(i); break;                                      /* ANDI */
	case 0x0D: dst = rt; v = s | I_UIMM(i); break;                                      /* ORI */
	case 0x0E: dst = rt; v = s ^ I_UIMM(i); break;                                      /* XORI */
	case 0x0F: dst = rt; v = salsa64_sx32(i << 16); break;                              /* LUI */
	case 0x19:                                                                           /* DADDIU */
		if (!cpu->allow64)
			return STEP_NEVER;
		dst = rt;
		v = s + imm;
		break;
	case 0x20: size = 1; break;                                                          /* LB */
	case 0x21: size = 2; break;                                                          /* LH */
	case 0x23: size = 4; break;                                                          /* LW */
	case 0x24: size = 1; sign = false; break;                                            /* LBU */
	case 0x25: size = 2; sign = false; break;                                            /* LHU */
	case 0x27: if (!cpu->allow64) return STEP_NEVER; size = 4; sign = false; break;     /* LWU */
	case 0x37: if (!cpu->allow64) return STEP_NEVER; size = 8; break;                   /* LD */
	default:
		return STEP_NEVER;
	}
	if (size) {
		/* Una carga fuera de la RDRAM (E/S, ROM) o que fallaría no se simula. */
		if (!translate(cpu, s + imm, size, &pa))
			return STEP_NEVER;
		dst = rt;
		v = load(cpu, pa, size, sign);
	}
	if (dst)
		g[dst] = v;
	return STEP_OK;

branch:
	*target = pc + 4 + (imm << 2);
	return STEP_BRANCH;
}

/* Una vuelta desde el estado actual: delay slot de `branch_pc`, cuerpo desde
 * `target` y el salto. */
static enum verdict analyze(const struct salsa64_cpu *cpu, uint64_t branch_pc, uint64_t target)
{
	uint64_t g[32], to;
	uint32_t i;
	bool taken;

	memcpy(g, cpu->gpr, sizeof g);
	uint64_t pc = branch_pc + 4;
	for (;;) {
		if (!fetch(cpu, pc, &i))
			return NEVER;
		enum step st = simulate(cpu, g, i, pc, &taken, &to);
		if (pc == branch_pc) {
			if (st != STEP_BRANCH)
				return st == STEP_NOT_YET ? NOT_YET : NEVER;
			if (!taken || to != target)
				return NOT_YET;
			break;
		}
		if (st == STEP_BRANCH || st == STEP_NEVER)
			return NEVER;  /* salto en el cuerpo o en el delay slot, u operación no simulable */
		if (st == STEP_NOT_YET)
			return NOT_YET;
		pc = pc == branch_pc + 4 ? target : pc + 4;
	}
	return memcmp(g, cpu->gpr, sizeof g) == 0 ? IDLE_LOOP : NOT_YET;
}

void salsa64_vr4300_idle(struct salsa64_cpu *cpu, uint64_t branch_pc, uint64_t target, uint32_t instr)
{
	unsigned slot = (unsigned)(branch_pc >> 2) & (SALSA64_IDLE_CACHE - 1);
	uint32_t key = (uint32_t)branch_pc ^ instr;

	if (cpu->idle_off) {
		cpu->idle_reject[slot] = key;  /* no volver a preguntar hasta reactivarlo */
		return;
	}
	if (cpu->delay_slot)  /* salto dentro de un delay slot: no es un bucle normal */
		return;
	switch (analyze(cpu, branch_pc, target)) {
	case NEVER:
		cpu->idle_reject[slot] = key;
		return;
	case NOT_YET:
		if (cpu->idle_strike[slot] == key)
			cpu->idle_reject[slot] = key;
		else
			cpu->idle_strike[slot] = key;
		return;
	case IDLE_LOOP:
		cpu->idle_strike[slot] = 0;
		break;
	}

	/* Vueltas enteras hasta el próximo evento: el salto vuelve a ejecutarse
	 * a lo sumo en el ciclo del evento, como sin saltar. Cada instrucción
	 * cuesta un ciclo (vr4300.h); con un modelo de costos, aquí iría el
	 * costo de una vuelta. */
	uint64_t next = cpu->sched->next, now = cpu->cycles;
	uint64_t len = (branch_pc - target) / 4 + 2;
	if (next == SALSA64_SCHED_NEVER || next <= now)
		return;
	uint64_t skip = (next - now) / len * len;
	if (!skip)
		return;
	cpu->cycles = now + skip;
	cpu->idle_cycles += skip;
	cpu->pc = branch_pc + 4;
	cpu->next_pc = target;
	cpu->branch = 1;
	cpu->redirect = 1;
}

void salsa64_cpu_set_idle(struct salsa64_cpu *cpu, bool on)
{
	cpu->idle_off = !on;
	memset(cpu->idle_reject, 0, sizeof cpu->idle_reject);
	memset(cpu->idle_strike, 0, sizeof cpu->idle_strike);
	/* El recompilador decidió al compilar qué saltos consultan la tabla
	 * (rec_emit.c, idle_candidate): con la tabla vacía, todo de nuevo. */
	if (cpu->rec)
		salsa64_rec_flush_why(cpu->rec, SALSA64_REC_FLUSH_IDLE);
}
