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
 *     modo kernel, por KSEG0/KSEG1 o por la TLB (nada de E/S salvo el MI,
 *     COP0 salvo lo de abajo, ni FPU),
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
 * Vueltas largas (2026-10-16): el hilo ocioso de varios juegos no es un
 * `b .` sino `for (;;) osYieldThread();` (Doom 64, 1080, NFL Blitz, Indiana
 * Jones): cada vuelta pasa por el planificador de libultra (__osDisableInt,
 * guardar el contexto, encolarse, __osDispatchThread con MTC0 Status y EPC,
 * la máscara del MI, ERET y __osRestoreInt), unas 140 instrucciones que
 * dejan todo como estaba si no hay otro hilo listo. La simulación sigue
 * entonces saltos, llamadas y regresos (hasta SIM_STEPS instrucciones) y
 * acepta además, sobre copias:
 *   - almacenamientos en la RDRAM (en una tabla aparte que leen las cargas
 *     de la vuelta); al final cada byte escrito tiene que valer lo que valía
 *     al empezar (guardar el contexto y encolarse escriben lo mismo, o algo
 *     que la misma vuelta deshace);
 *   - MFC0 de Status, Cause y EPC, MTC0 de Status (sin cambiar más que IE,
 *     EXL, ERL e IM, y sin salir del modo kernel) y de EPC, y ERET;
 *   - MI_INTR_MASK leído y escrito (pares limpiar/poner), y MI_MODE,
 *     MI_VERSION y MI_INTR leídos (solo cambian en un evento);
 *   - MULT/MULTU/DIV/DIVU, MTHI/MTLO, MFHI/MFLO y los saltos de enlace;
 *   - de la FPU, lo de guardar y reponer un hilo que la usa: CFC1/CTC1 de
 *     FCR31 y LWC1/LDC1/SWC1/SDC1 (con CU1; FR no cambia en la vuelta).
 * Si en algún punto quedaría una interrupción habilitada y pendiente, no es
 * de espera (el CPU la tomaría ahí). El punto fijo se exige también en HI,
 * LO, Status, EPC, el latch de COP0, LLbit, la máscara del MI, FCR31 y los
 * registros de la FPU. Cada instrucción ejecutada cuesta un ciclo (las de
 * un delay slot anulado por un branch-likely no se ejecutan ni cuestan),
 * así que la vuelta dura lo que contó la simulación. Código que la misma
 * vuelta escribió no se simula.
 *
 * Escrito desde la documentación del VR4300; la idea de saltar el bucle
 * ocioso es común a los emuladores de N64, el criterio de punto fijo es
 * propio.
 */
#include <string.h>

#include "core/bus.h"
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
	STEP_BRANCH,  /* salto evaluado: `taken` y `target` (y `likely`) */
	STEP_ERET,    /* ERET: sigue en `target`, sin delay slot */
	STEP_NOT_YET,
	STEP_NEVER,
};

#define SIM_STEPS  1024u  /* instrucciones por vuelta como mucho */
#define IDLE_FAILS_PROVEN 64u  /* fallos seguidos que aguanta un salto que ya fue de espera */
#define SIM_STORES 96u    /* palabras dobles distintas escritas en una vuelta */
#define MI_PA      0x04300000u
#define CAUSE_IP2  0x0400u
#define STATUS_WMASK 0xFF57FFFFu  /* cop0.c */

/* Lo que una vuelta puede cambiar, en copia. Los almacenamientos en la
 * RDRAM van a `st` (por palabra doble alineada, con los bytes válidos). */
struct sim {
	const struct salsa64_cpu *cpu;
	uint64_t g[32], hi, lo, epc, latch;
	uint64_t fpr[32];
	uint32_t status, mi_mask, fcr31;
	uint8_t llbit;
	unsigned nst;
	struct { uint32_t pa8; uint8_t valid; uint8_t b[8]; } st[SIM_STORES];
};

/* La traducción sin efectos en modo kernel, alineada y dentro de la RDRAM:
 * KSEG0/KSEG1 restando la base (el camino rápido del intérprete) y lo demás
 * por la TLB, sin excepciones (si el intérprete lanzaría una, no es un
 * bucle de espera). Con `store`, la página tiene que estar sucia. `mi`
 * acepta además los registros del MI por KSEG0/KSEG1. */
static bool translate_sim(const struct sim *s, uint64_t va, uint32_t size, bool store, bool mi, uint32_t *pa)
{
	const struct salsa64_cpu *cpu = s->cpu;
	if ((va & (size - 1)) || cpu->mode != SALSA64_MODE_KERNEL)
		return false;
	uint32_t p;
	if ((va >> 30) == 0x3FFFFFFFEull) {
		p = (uint32_t)va & 0x1FFFFFFF;
		if (mi && size == 4 && p >= MI_PA && p < MI_PA + 16) {
			*pa = p;
			return true;
		}
	} else {
		bool dirty;
		/* Con ERL distinto del de la CPU, kuseg se traduciría distinto. */
		if (((s->status ^ cpu->status) & SALSA64_ST_ERL) && (uint32_t)va < 0x80000000u)
			return false;
		if (!salsa64_vr4300_kernel_probe(cpu, va, &p, &dirty) || (store && !dirty))
			return false;
	}
	if (p >= cpu->rdram_size || cpu->rdram_size - p < size)
		return false;
	*pa = p;
	return true;
}

static int find_store(const struct sim *s, uint32_t pa8)
{
	for (unsigned k = 0; k < s->nst; k++)
		if (s->st[k].pa8 == pa8)
			return (int)k;
	return -1;
}

static bool fetch(const struct sim *s, uint64_t va, uint32_t *instr)
{
	uint32_t pa;
	if (!translate_sim(s, va, 4, false, false, &pa))
		return false;
	int e = find_store(s, pa & ~7u);
	if (e >= 0 && (s->st[e].valid >> (pa & 7) & 0xF))
		return false;  /* código escrito en esta vuelta */
	*instr = salsa64_rd32(s->cpu->rdram + pa);
	return true;
}

/* Carga de la RDRAM vista por la vuelta: los bytes ya escritos salen de la
 * tabla. Alineada, así cae dentro de una palabra doble. */
static uint64_t load(const struct sim *s, uint32_t pa, uint32_t size, bool sign)
{
	int e = find_store(s, pa & ~7u);
	uint64_t v = 0;
	for (uint32_t j = 0; j < size; j++) {
		unsigned off = (pa & 7) + j;
		uint8_t b = e >= 0 && (s->st[e].valid >> off & 1) ? s->st[e].b[off] : s->cpu->rdram[pa + j];
		v = v << 8 | b;
	}
	if (sign)
		v = size == 1 ? salsa64_sx8((uint8_t)v) : size == 2 ? salsa64_sx16((uint16_t)v)
		  : size == 4 ? salsa64_sx32((uint32_t)v) : v;
	return v;
}

static bool store(struct sim *s, uint32_t pa, uint32_t size, uint64_t v)
{
	int e = find_store(s, pa & ~7u);
	if (e < 0) {
		if (s->nst == SIM_STORES)
			return false;
		e = (int)s->nst++;
		s->st[e].pa8 = pa & ~7u;
		s->st[e].valid = 0;
	}
	for (uint32_t j = 0; j < size; j++) {
		unsigned off = (pa & 7) + j;
		s->st[e].b[off] = (uint8_t)(v >> (8 * (size - 1 - j)));
		s->st[e].valid |= (uint8_t)(1u << off);
	}
	return true;
}

/* Cause con la línea del MI según la máscara de la vuelta. */
static uint32_t cause_sim(const struct sim *s)
{
	uint32_t c = s->cpu->cause & ~CAUSE_IP2;
	return (s->cpu->bus->mi.intr & s->mi_mask) ? c | CAUSE_IP2 : c;
}

/* ¿Tomaría el CPU una interrupción con el estado de la vuelta? */
static bool irq_sim(const struct sim *s)
{
	return (cause_sim(s) & s->status & SALSA64_ST_IM) != 0 &&
	       (s->status & (SALSA64_ST_IE | SALSA64_ST_EXL | SALSA64_ST_ERL)) == SALSA64_ST_IE;
}

/* Sigue en modo kernel (el único en que se analiza). */
static bool kernel_sim(uint32_t st)
{
	return (st & (SALSA64_ST_EXL | SALSA64_ST_ERL)) || !(st & SALSA64_ST_KSU);
}

static enum step cop0(struct sim *s, uint32_t i, uint64_t *target)
{
	unsigned rt = I_RT(i), rd = I_RD(i);
	uint64_t v;
	switch (I_RS(i)) {
	case 0x00:                                                                   /* MFC0 */
		switch (rd) {
		case SALSA64_CP0_STATUS: v = s->status; break;
		case SALSA64_CP0_CAUSE:  v = cause_sim(s); break;
		case SALSA64_CP0_EPC:    v = s->epc; break;
		default:                 return STEP_NEVER;  /* Count y Random cambian solos */
		}
		if (rt)
			s->g[rt] = salsa64_sx32((uint32_t)v);
		return STEP_OK;
	case 0x04:                                                                   /* MTC0 */
		v = s->g[rt];
		if (rd == SALSA64_CP0_STATUS) {
			uint32_t st = (uint32_t)v & STATUS_WMASK;
			if (((st ^ s->status) & ~(SALSA64_ST_IE | SALSA64_ST_EXL | SALSA64_ST_ERL | SALSA64_ST_IM)) ||
			    !kernel_sim(st))
				return STEP_NEVER;
			s->status = st;
		} else if (rd == SALSA64_CP0_EPC) {
			s->epc = v;
		} else {
			return STEP_NEVER;
		}
		s->latch = v;
		return STEP_OK;
	default:
		if (I_RS(i) >= 0x10 && I_FUNCT(i) == 0x18) {                         /* ERET */
			if (s->status & SALSA64_ST_ERL) {
				*target = s->cpu->error_epc;
				s->status &= ~SALSA64_ST_ERL;
			} else {
				*target = s->epc;
				s->status &= ~SALSA64_ST_EXL;
			}
			s->llbit = 0;
			return kernel_sim(s->status) ? STEP_ERET : STEP_NEVER;
		}
		return STEP_NEVER;
	}
}

/* Registros del MI: lectura sin efectos; solo MI_INTR_MASK se escribe (en
 * la copia, con los pares limpiar/poner de mi.c). */
static bool mi_access(struct sim *s, uint32_t pa, bool is_store, uint64_t *v)
{
	unsigned reg = (pa - MI_PA) >> 2;
	if (is_store) {
		if (reg != 3)
			return false;
		for (int k = 0; k < 6; k++) {
			if (*v & (1u << (2 * k)))
				s->mi_mask &= ~(1u << k);
			if (*v & (2u << (2 * k)))
				s->mi_mask |= 1u << k;
		}
		return true;
	}
	*v = salsa64_sx32(reg == 3 ? s->mi_mask : salsa64_mi_read(s->cpu->bus, reg << 2));
	return true;
}

/* COP1 de un cambio de contexto (guardar y reponer la FPU de un hilo):
 * CFC1/CTC1 de FCR31 y los accesos de 32 y 64 bits a memoria, con la
 * semántica de Status.FR de vr4300_internal.h (FR no cambia en la vuelta). */
static unsigned fpr64(const struct sim *s, unsigned r)
{
	return (s->status & SALSA64_ST_FR) ? r : r & ~1u;
}

static uint32_t fpr_get32(const struct sim *s, unsigned r)
{
	if ((s->status & SALSA64_ST_FR) || !(r & 1))
		return (uint32_t)s->fpr[r];
	return (uint32_t)(s->fpr[r & ~1u] >> 32);
}

static void fpr_set32(struct sim *s, unsigned r, uint32_t v)
{
	if ((s->status & SALSA64_ST_FR) || !(r & 1))
		s->fpr[r] = (s->fpr[r] & 0xFFFFFFFF00000000ull) | v;
	else
		s->fpr[r & ~1u] = (s->fpr[r & ~1u] & 0xFFFFFFFFull) | (uint64_t)v << 32;
}

#define FCR31_WMASK 0x0183FFFFu  /* cop1.c */

static enum step cop1(struct sim *s, uint32_t i)
{
	unsigned rt = I_RT(i), fs = I_FS(i);
	if (I_FMT(i) == 0x02) {                                                      /* CFC1 */
		if (rt)
			s->g[rt] = salsa64_sx32(fs == 0 ? SALSA64_FCR0_VALUE : fs == 31 ? s->fcr31 : 0);
		return STEP_OK;
	}
	if (I_FMT(i) == 0x06) {                                                      /* CTC1 */
		if (fs != 31)
			return STEP_OK;
		uint32_t f = (uint32_t)s->g[rt] & FCR31_WMASK;
		unsigned cause = (f >> 12) & 0x3F;
		if (cause & (((f >> 7) & 0x1F) | 0x20))
			return STEP_NOT_YET;  /* lanzaría la excepción de la FPU */
		s->fcr31 = f;
		return STEP_OK;
	}
	return STEP_NEVER;
}

/* Simula la instrucción `i` en `pc` sobre la copia. Las que podrían lanzar
 * una excepción por sus valores (desbordamiento) devuelven STEP_NOT_YET: que
 * la ejecute el intérprete. */
static enum step simulate(struct sim *sm, uint32_t i, uint64_t pc, bool *taken, bool *likely, uint64_t *target)
{
	const struct salsa64_cpu *cpu = sm->cpu;
	uint64_t *g = sm->g;
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i), sa = I_SA(i), dst;
	uint64_t s = g[rs], t = g[rt], v, imm = I_SIMM(i);
	uint32_t size = 0, pa;
	bool sign = true, st = false, fp = false;

	*likely = false;
	if ((I_OP(i) == 0x11 || (I_OP(i) & 0x33) == 0x31) && !(sm->status & SALSA64_ST_CU1))
		return STEP_NEVER;  /* COP1 inusable: excepción */
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
		case 0x08:                                                                   /* JR */
			*taken = true;
			*target = s;
			return STEP_BRANCH;
		case 0x09:                                                                   /* JALR */
			*taken = true;
			*target = s;
			if (rd)
				g[rd] = pc + 8;
			return STEP_BRANCH;
		case 0x0F: return STEP_OK;                                                   /* SYNC */
		case 0x10: v = sm->hi; break;                                                /* MFHI */
		case 0x11: sm->hi = s; return STEP_OK;                                       /* MTHI */
		case 0x12: v = sm->lo; break;                                                /* MFLO */
		case 0x13: sm->lo = s; return STEP_OK;                                       /* MTLO */
		case 0x18: {                                                                 /* MULT (como interp.c) */
			uint64_t p = s * (uint64_t)((int64_t)(t << 29) >> 29);
			sm->lo = salsa64_sx32((uint32_t)p);
			sm->hi = salsa64_sx32((uint32_t)(p >> 32));
			return STEP_OK;
		}
		case 0x19: {                                                                 /* MULTU */
			uint64_t p = (uint64_t)(uint32_t)s * (uint32_t)t;
			sm->lo = salsa64_sx32((uint32_t)p);
			sm->hi = salsa64_sx32((uint32_t)(p >> 32));
			return STEP_OK;
		}
		case 0x1A: {                                                                 /* DIV */
			int32_t a = (int32_t)s, b = (int32_t)t;
			if (b == 0) {
				sm->lo = a < 0 ? 1 : ~0ull;
				sm->hi = salsa64_sx32((uint32_t)a);
			} else {
				int64_t q = (int64_t)a / b, r = (int64_t)a % b;
				sm->lo = salsa64_sx32((uint32_t)q);
				sm->hi = salsa64_sx32((uint32_t)r);
			}
			return STEP_OK;
		}
		case 0x1B: {                                                                 /* DIVU */
			uint32_t a = (uint32_t)s, b = (uint32_t)t;
			sm->lo = b ? salsa64_sx32(a / b) : ~0ull;
			sm->hi = salsa64_sx32(b ? a % b : a);
			return STEP_OK;
		}
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
		if ((rt & ~0x13u) != 0)  /* BLTZ, BGEZ, BLTZL, BGEZL y las de enlace */
			return STEP_NEVER;
		*taken = (rt & 1) ? (int64_t)s >= 0 : (int64_t)s < 0;
		*likely = (rt & 2) != 0;
		*target = pc + 4 + (imm << 2);
		if (rt & 0x10)
			g[31] = pc + 8;  /* se escribe se tome o no */
		return STEP_BRANCH;
	case 0x02:                                                                           /* J */
	case 0x03:                                                                           /* JAL */
		*taken = true;
		*target = ((pc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		if (I_OP(i) == 0x03)
			g[31] = pc + 8;
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
	case 0x10: return cop0(sm, i, target);                                              /* COP0 */
	case 0x11: return cop1(sm, i);                                                      /* COP1 */
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
	case 0x28: size = 1; st = true; break;                                               /* SB */
	case 0x29: size = 2; st = true; break;                                               /* SH */
	case 0x2B: size = 4; st = true; break;                                               /* SW */
	case 0x3F: if (!cpu->allow64) return STEP_NEVER; size = 8; st = true; break;        /* SD */
	case 0x31: size = 4; fp = true; break;                                               /* LWC1 */
	case 0x35: size = 8; fp = true; break;                                               /* LDC1 */
	case 0x39: size = 4; fp = true; st = true; break;                                    /* SWC1 */
	case 0x3D: size = 8; fp = true; st = true; break;                                    /* SDC1 */
	default:
		return STEP_NEVER;
	}
	if (fp) {
		if (!translate_sim(sm, s + imm, size, st, false, &pa))
			return STEP_NEVER;
		if (st)
			return store(sm, pa, size, size == 8 ? sm->fpr[fpr64(sm, rt)] : fpr_get32(sm, rt)) ? STEP_OK
												     : STEP_NEVER;
		v = load(sm, pa, size, false);
		if (size == 8)
			sm->fpr[fpr64(sm, rt)] = v;
		else
			fpr_set32(sm, rt, (uint32_t)v);
		return STEP_OK;
	}
	if (size) {
		/* Un acceso fuera de la RDRAM (E/S salvo el MI, ROM) o que fallaría
		 * no se simula. */
		if (!translate_sim(sm, s + imm, size, st, true, &pa))
			return STEP_NEVER;
		if (pa >= MI_PA && pa < MI_PA + 16) {
			v = t;
			if (!mi_access(sm, pa, st, &v))
				return STEP_NEVER;
			if (st)
				return STEP_OK;
		} else if (st) {
			return store(sm, pa, size, t) ? STEP_OK : STEP_NEVER;
		} else {
			v = load(sm, pa, size, sign);
		}
		dst = rt;
	}
	if (dst)
		g[dst] = v;
	return STEP_OK;

branch:
	*likely = (I_OP(i) & 0x10) != 0;
	*target = pc + 4 + (imm << 2);
	return STEP_BRANCH;
}

static struct sim sim;  /* grande para la pila; el CPU corre en un solo hilo */

/* ¿Volvió todo a como estaba al empezar la vuelta? */
static bool fixed_point(const struct sim *s)
{
	const struct salsa64_cpu *cpu = s->cpu;
	if (memcmp(s->g, cpu->gpr, sizeof s->g) || s->hi != cpu->hi || s->lo != cpu->lo || s->status != cpu->status ||
	    s->epc != cpu->epc || s->latch != cpu->cop0_latch || s->llbit != cpu->llbit || s->mi_mask != cpu->bus->mi.mask ||
	    s->fcr31 != cpu->fcr31 || memcmp(s->fpr, cpu->fpr, sizeof s->fpr))
		return false;
	for (unsigned k = 0; k < s->nst; k++)
		for (unsigned off = 0; off < 8; off++)
			if ((s->st[k].valid >> off & 1) && s->st[k].b[off] != cpu->rdram[s->st[k].pa8 + off])
				return false;
	return true;
}

/* Una vuelta desde el estado actual: delay slot de `branch_pc`, cuerpo desde
 * `target` (con sus saltos, llamadas y regresos) hasta tomar de nuevo el
 * salto en `branch_pc` hacia `target`. `*len` es lo que duró, en ciclos. */
static enum verdict analyze(const struct salsa64_cpu *cpu, uint64_t branch_pc, uint64_t target, uint64_t *len)
{
	struct sim *s = &sim;
	s->cpu = cpu;
	memcpy(s->g, cpu->gpr, sizeof s->g);
	s->hi = cpu->hi;
	s->lo = cpu->lo;
	s->epc = cpu->epc;
	s->latch = cpu->cop0_latch;
	s->status = cpu->status;
	s->mi_mask = cpu->bus->mi.mask;
	s->llbit = cpu->llbit;
	memcpy(s->fpr, cpu->fpr, sizeof s->fpr);
	s->fcr31 = cpu->fcr31;
	s->nst = 0;

	/* Lo que no se puede simular descarta el salto para siempre si está en
	 * todos los caminos de la vuelta; después de un salto condicional quizá
	 * solo esté en el camino de esta vez (llegó un mensaje y el hilo lo
	 * atiende): esta vez no. */
	uint64_t pc = branch_pc + 4, after_ds = target, to = 0;
	bool in_ds = true, taken = false, likely = false, cond = false;
	uint32_t i;
	if (irq_sim(s))
		return NOT_YET;
	for (unsigned n = 1;; n++) {
		if (n > SIM_STEPS || !fetch(s, pc, &i))
			return cond ? NOT_YET : NEVER;
		enum step st = simulate(s, i, pc, &taken, &likely, &to);
		if (st == STEP_NOT_YET)
			return NOT_YET;
		if (st == STEP_NEVER)
			return cond ? NOT_YET : NEVER;
		if (pc == branch_pc && !in_ds) {
			/* El salto del bucle: otra vuelta si se vuelve a tomar igual. */
			if (st != STEP_BRANCH)
				return NEVER;
			if (!taken || to != target)
				return NOT_YET;
			*len = n;
			return fixed_point(s) ? IDLE_LOOP : NOT_YET;
		}
		if (in_ds && st != STEP_OK)
			return NEVER;  /* un salto o ERET en un delay slot */
		if (irq_sim(s))
			return NOT_YET;  /* el CPU tomaría una interrupción */
		if (st == STEP_OK) {
			pc = in_ds ? after_ds : pc + 4;
			in_ds = false;
		} else if (st == STEP_ERET) {
			pc = to;
		} else {
			cond |= I_OP(i) == 0x01 || I_OP(i) >= 0x04;  /* no J, JAL, JR ni JALR */
			if (likely && !taken) {
				pc += 8;  /* delay slot anulado: ni se ejecuta ni cuesta */
			} else {
				after_ds = taken ? to : pc + 8;
				pc += 4;
				in_ds = true;
			}
		}
	}
}

void salsa64_vr4300_idle(struct salsa64_cpu *cpu, uint64_t branch_pc, uint64_t target, uint32_t instr)
{
	unsigned slot = SALSA64_IDLE_SLOT(branch_pc);
	uint32_t key = (uint32_t)branch_pc ^ instr;

	if (cpu->idle_off) {
		cpu->idle_reject[slot] = key;  /* no volver a preguntar hasta reactivarlo */
		return;
	}
	if (cpu->delay_slot)  /* salto dentro de un delay slot: no es un bucle normal */
		return;
	/* Un salto se descarta al segundo fallo seguido (el primero puede ser el
	 * delay slot o la primera vuelta); uno que ya fue de espera, recién al
	 * IDLE_FAILS_PROVEN: entre evento y evento la espera de un hilo que
	 * recibe mensajes falla mientras los atiende. */
	uint64_t len = 0;
	enum verdict v = analyze(cpu, branch_pc, target, &len);
	bool proven = cpu->idle_proven[slot] == key;
	if (v == NEVER && !proven) {
		cpu->idle_reject[slot] = key;
		return;
	}
	if (v != IDLE_LOOP) {
		if (cpu->idle_strike[slot] != key) {
			cpu->idle_strike[slot] = key;
			cpu->idle_fails[slot] = 0;
		}
		if (++cpu->idle_fails[slot] >= (proven ? IDLE_FAILS_PROVEN : 2))
			cpu->idle_reject[slot] = key;
		return;
	}
	cpu->idle_strike[slot] = key;
	cpu->idle_fails[slot] = 0;
	cpu->idle_proven[slot] = key;

	/* Vueltas enteras hasta el próximo evento: el salto vuelve a ejecutarse
	 * a lo sumo en el ciclo del evento, como sin saltar. Cada instrucción
	 * cuesta un ciclo (vr4300.h); con un modelo de costos, aquí iría el
	 * costo de una vuelta. */
	uint64_t next = cpu->sched->next, now = cpu->cycles;
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
	memset(cpu->idle_proven, 0, sizeof cpu->idle_proven);
	memset(cpu->idle_fails, 0, sizeof cpu->idle_fails);
	/* El recompilador decidió al compilar qué saltos consultan la tabla
	 * (rec_emit.c, idle_candidate): con la tabla vacía, todo de nuevo. */
	if (cpu->rec)
		salsa64_rec_flush_why(cpu->rec, SALSA64_REC_FLUSH_IDLE);
}
