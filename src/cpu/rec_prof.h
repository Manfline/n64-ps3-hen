/*
 * Salsa64 — perfil del recompilador: dónde se va el tiempo del hilo de
 * emulación cuando corre el código generado, por causa.
 *
 * Se activa poniendo rec->prof (NULL = apagado). Con él apagado el costo es
 * una carga y un salto por llamada a C desde el código generado y por vuelta
 * del despachador; encendido, dos lecturas del timebase (`mftb`, 79,8 MHz en
 * la PS3) por tramo medido. El código generado no se toca: su tiempo es el
 * de cada entrada menos lo que tardaron las llamadas a C que hizo.
 *
 * En un host que no es ppc64 los ticks valen 0 y solo sirven las cuentas
 * (en qemu los ticks existen pero no representan el Cell).
 */
#ifndef SALSA64_CPU_REC_PROF_H
#define SALSA64_CPU_REC_PROF_H

#include <stddef.h>

#include "core/types.h"

enum salsa64_rec_prof_item {
	/* Código generado (entradas menos sus llamadas a C). */
	SALSA64_PROF_NATIVE,
	/* Cargas por el camino lento (rec_h_load), según la dirección. */
	SALSA64_PROF_LOAD_TLB_RDRAM,  /* mapeada por la TLB, a la RDRAM */
	SALSA64_PROF_LOAD_TLB_IO,     /* mapeada por la TLB, fuera de la RDRAM */
	SALSA64_PROF_LOAD_TLB_FAULT,  /* la TLB no la resolvió: excepción */
	SALSA64_PROF_LOAD_IO,         /* KSEG0/KSEG1 fuera de la RDRAM (RCP, PIF, ROM) */
	SALSA64_PROF_LOAD_ADDR,       /* sin alinear o de 64 bits: error de dirección */
	/* Almacenamientos por el camino lento (rec_h_store). */
	SALSA64_PROF_STORE_TLB_RDRAM,
	SALSA64_PROF_STORE_TLB_IO,
	SALSA64_PROF_STORE_TLB_FAULT,
	SALSA64_PROF_STORE_IO,
	SALSA64_PROF_STORE_CODE,      /* RDRAM con código recompilado en esos 128 bytes */
	SALSA64_PROF_STORE_ADDR,
	/* Instrucciones que el código generado le pasa al intérprete (rec_h_exec). */
	SALSA64_PROF_EXEC_COP0,       /* MTC0, TLB, ERET, MFC0 de Random */
	SALSA64_PROF_EXEC_FPU,        /* COP1 por el camino completo */
	SALSA64_PROF_EXEC_LLSC,       /* LL, LLD, SC, SCD */
	SALSA64_PROF_EXEC_CACHE,
	SALSA64_PROF_EXEC_TRAP,       /* traps (SPECIAL y REGIMM) */
	SALSA64_PROF_EXEC_OTHER,      /* COP2, reservadas, no alineadas con RARE_EXEC */
	/* Excepciones desde el código generado (SYSCALL, BREAK, desbordamiento). */
	SALSA64_PROF_EXCEPTION,
	/* El intérprete en el despachador, por qué no corrió un bloque. */
	SALSA64_PROF_INTERP_START,    /* el bloque salió en la entrada (no cabía antes del evento) */
	SALSA64_PROF_INTERP_MID,      /* el bloque salió a mitad (evento tras una llamada a C, invalidación) */
	SALSA64_PROF_INTERP_IRQ,      /* interrupción pendiente */
	SALSA64_PROF_INTERP_DS,       /* delay slot pendiente (tras un bucle de espera o una excepción) */
	SALSA64_PROF_INTERP_MODE,     /* modo usuario o supervisor */
	SALSA64_PROF_INTERP_SEG,      /* código que no se compila (SP DMEM, PIF, 64 bits…) */
	SALSA64_PROF_INTERP_NOCODE,   /* el bloque no se pudo compilar */
	SALSA64_PROF_INTERP_NOFIT,    /* el bloque no cabe antes del evento (desde el despachador) */
	/* Eventos del planificador (salsa64_sched_dispatch), por tipo; el del
	 * RSP incluye el sondeo del SPE y lo que duerme esperándolo. */
	SALSA64_PROF_EV_STOP,
	SALSA64_PROF_EV_COMPARE,
	SALSA64_PROF_EV_VI_INTR,
	SALSA64_PROF_EV_VI_FIELD,
	SALSA64_PROF_EV_SP_TASK,
	SALSA64_PROF_EV_DP_SYNC,
	SALSA64_PROF_EV_AI_DMA,
	SALSA64_PROF_EV_RSP_LLE,
	SALSA64_PROF_EV_MANY,         /* varios vencidos a la vez */
	/* Lo demás del despachador. */
	SALSA64_PROF_COMPILE,         /* compilar bloques (también los recortados) */
	SALSA64_PROF_XSYNC,           /* revisar vtlb[] tras un cambio de la TLB */
	SALSA64_PROF_IDLE,            /* análisis de bucles de espera (idle.c) y el salto de tiempo */
	SALSA64_PROF_DISPATCH,        /* el resto: buscar el bloque, enlazar, mirar banderas */
	/* Salidas del código generado, solo cuentas (en el orden de
	 * SALSA64_REC_EXIT_* de rec.h). */
	SALSA64_PROF_EXIT_LINK,
	SALSA64_PROF_EXIT_MID,
	SALSA64_PROF_EXIT_JUMP,
	SALSA64_PROF_EXIT_IDLE,
	SALSA64_PROF_EXIT_START,
	SALSA64_PROF_EXIT_END,
	SALSA64_PROF_EXIT_REDIRECT,   /* excepción o ERET dentro del bloque */
	SALSA64_PROF_N
};

/* Perfil por bloque (solo cuentas; lo pide el host, salsa64-run con
 * SALSA64_REC_PROF): cuántas veces pasó la ejecución por el cuerpo de cada
 * bloque (el código generado incrementa hits[id]), qué usa cada uno, y una
 * tabla por pc que acumula lo de los bloques que murieron. Son 2,3 MB: no se
 * reserva en la PS3. */
#define SALSA64_PROF_BLOCKS 65536u
#define SALSA64_PROF_PCS    65536u
struct salsa64_rec_prof_block {
	uint64_t regs;     /* registros MIPS que lee o escribe (bit 32 HI, 33 LO) */
	uint32_t pc;
	uint16_t n;        /* instrucciones; 0 = sin bloque */
	uint8_t fpu;       /* operaciones de COP1 (formato S o D) */
	uint8_t mem;       /* cargas y almacenamientos */
};
struct salsa64_rec_prof_pc {
	uint64_t hits, regs;
	uint32_t pc;
	uint16_t n, id;    /* id si está vivo (para listar sus palabras), 0 si no */
	uint8_t fpu, mem;
};
struct salsa64_rec_prof_blocks {
	uint64_t hits[SALSA64_PROF_BLOCKS];
	struct salsa64_rec_prof_block info[SALSA64_PROF_BLOCKS];
	struct salsa64_rec_prof_pc pcs[SALSA64_PROF_PCS];
	uint32_t npcs;
};

struct salsa64_rec_prof {
	uint64_t t[SALSA64_PROF_N];   /* ticks del timebase */
	uint64_t n[SALSA64_PROF_N];   /* cuentas (instrucciones en INTERP_*, llamadas en lo demás) */
	uint64_t nested;              /* suma de lo medido dentro del código generado */
	uint64_t total;               /* ticks dentro de salsa64_rec_run */
	struct salsa64_rec_prof_blocks *blocks;  /* NULL = sin perfil por bloque */
};

static inline uint64_t salsa64_rec_prof_now(void)
{
#if defined(__powerpc64__)
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
#else
	return 0;
#endif
}

/* Cuenta `dt` ticks en `item`, también como anidados (dentro del código
 * generado). */
static inline void salsa64_rec_prof_add(struct salsa64_rec_prof *p, unsigned item, uint64_t dt)
{
	p->t[item] += dt;
	p->n[item]++;
	p->nested += dt;
}

/* Texto con el desglose de lo acumulado entre `prev` y `now` (prev se
 * actualiza): tiempo en ms (o % del total) y cuentas, en pocas líneas
 * separadas por '\n'. `hz` es la frecuencia del timebase (0: solo cuentas);
 * `fields` reparte las cuentas por campo si no es 0. */
void salsa64_rec_prof_report(const struct salsa64_rec_prof *now, struct salsa64_rec_prof *prev, uint64_t hz,
			     unsigned fields, char *buf, size_t size);

struct salsa64_rec;
/* Perfil por bloque: suma hits[id] a la entrada por pc de su bloque (que va
 * a morir, o sigue vivo) y lo deja en 0. */
void salsa64_rec_prof_fold(struct salsa64_rec_prof *p, unsigned id, bool alive);
/* Los `top` bloques más ejecutados (con sus palabras MIPS si siguen vivos:
 * tools/mips-disasm.py las lee) y el uso de registros pesado por ejecuciones,
 * una línea por llamada a `line`. */
void salsa64_rec_prof_blocks_report(struct salsa64_rec *rec, unsigned top, void (*line)(void *ctx, const char *text),
				    void *ctx);

#endif
