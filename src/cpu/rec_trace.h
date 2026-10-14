/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — registro del recompilador para el perfilado (docs/perfilado.md):
 * lo que la app de perfilado guarda de cada sesión para optimizar después.
 *
 * Se activa poniendo rec->trace antes de salsa64_rec_init (NULL = apagado:
 * el preámbulo y los bloques salen idénticos a los de siempre). Con él:
 *
 *   - Cada compilación se informa (on_block): dirección, palabra de entrada
 *     en la caché, instrucciones MIPS, palabras de PPC calientes y frías, y
 *     cuántas instrucciones y palabras van a cada clase (entrada, ALU,
 *     memoria, FPU, saltos, salida, otras), cómo termina el bloque (JR ra,
 *     JR, JALR, JAL, J, condicional…), su huella y si es una retraducción de
 *     algo ya visto en la sesión (misma dirección, huella y largo).
 *   - Desalojos de tramos y vaciados (on_evict) y, por campo, cuántos
 *     bloques se compilaron y cuánto tardó (field, lo pone a cero quien
 *     llama): la causa de los tirones por traducción.
 *   - Con `seq`, ventanas cortas de la secuencia de entradas a bloques y de
 *     las vueltas a C, en orden (para modelar la caché del PPE en la Mac,
 *     docs/cache-ppe.md). La entrada de cada bloque lleva una palabra más,
 *     `bne cr3` a un tramo frío (li r0, id; bl trace_rtn; b de vuelta): cr3
 *     dice si hay ventana abierta. El preámbulo lo pone al entrar desde C
 *     (cr3 es no volátil: las llamadas a C lo respetan) y trace_rtn lo apaga
 *     al llenarse. Incompatible con la FPU residente, que usa cr3: con
 *     SALSA64_REC_OPT_FPU_RES no se emite. Los bucles dentro de un bloque no
 *     pasan por su entrada: una vuelta no se registra.
 *   - Con `jr`, cada JR/JALR cuenta por su bloque (id) cuántas veces se
 *     ejecutó y cuántas fue al mismo destino que la vez anterior (lo que
 *     acertaría una caché de destinos de una entrada), en una rutina del
 *     preámbulo (exit_jr_count) antes de exit_jr: ~16 instrucciones por JR.
 *
 * Todo preasignado (unos 3 MB); nada del juego: direcciones, cuentas,
 * huellas de un solo sentido y tiempos.
 */
#ifndef SALSA64_CPU_REC_TRACE_H
#define SALSA64_CPU_REC_TRACE_H

#include "core/types.h"
#include "cpu/vr4300.h"

struct salsa64_rec;

#define SALSA64_RT_IDS       65536u
#define SALSA64_RT_WIN_MAX   32768u   /* entradas por ventana, a lo más */
#define SALSA64_RT_TAB_MAX   16384u   /* bloques distintos por ventana */
#define SALSA64_RT_SEEN      131072u  /* huellas de bloques ya compilados (retraducciones) */
#define SALSA64_RT_JR_SITES  16384u   /* JR por dirección (plegados) */

/* Clases de palabras emitidas. */
enum salsa64_rt_class {
	SALSA64_RT_C_ENTRY,   /* entrada: ¿cabe?, COP1, recarga de registros */
	SALSA64_RT_C_ALU,     /* enteros, HI/LO, desplazamientos */
	SALSA64_RT_C_MEM,     /* cargas y almacenamientos (también LWC1/SDC1…) */
	SALSA64_RT_C_FPU,     /* COP1 */
	SALSA64_RT_C_BRANCH,  /* saltos y su enlace (sin el delay slot) */
	SALSA64_RT_C_EXIT,    /* salida: escritura de registros, enlaces, trampolines */
	SALSA64_RT_C_OTHER,   /* COP0, CACHE, SYSCALL, al intérprete */
	SALSA64_RT_CLASSES
};

/* Cómo termina un bloque. */
enum salsa64_rt_end {
	SALSA64_RT_END_LIMIT,   /* sin salto: tope de instrucciones o de página */
	SALSA64_RT_END_JR_RA,   /* jr ra */
	SALSA64_RT_END_JR,      /* jr con otro registro */
	SALSA64_RT_END_JALR,
	SALSA64_RT_END_JAL,
	SALSA64_RT_END_J,
	SALSA64_RT_END_BRANCH,  /* condicional */
	SALSA64_RT_END_LIKELY,  /* condicional «likely» */
	SALSA64_RT_END_C,       /* instrucción que vuelve a C (COP0, SYSCALL…) */
	SALSA64_RT_ENDS
};
#define SALSA64_RT_END_LOOP 0x80u  /* el salto final vuelve al inicio del bloque */

/* Banderas de un bloque compilado. */
#define SALSA64_RT_F_RETRANS 1u   /* ya se había compilado en esta sesión (misma vpc, huella y largo) */
#define SALSA64_RT_F_HOT     2u   /* fue a la zona caliente del perfil persistente */
#define SALSA64_RT_F_FRDEP   4u   /* depende de Status.FR */
#define SALSA64_RT_F_NOCODE  8u   /* no se pudo compilar: lo corre el intérprete */

struct salsa64_rec_trace_block {
	uint64_t hash;        /* huella de las instrucciones MIPS (salsa64_rec_profile_hash) */
	uint32_t vpc, pa;
	uint32_t code;        /* palabra de entrada en la caché (0: sin código) */
	uint32_t ticks;       /* timebase compilándolo (0 fuera del PPE) */
	uint16_t id, hot, cold;  /* palabras de PPC: sección caliente y fría */
	uint8_t n;            /* instrucciones MIPS */
	uint8_t end;          /* enum salsa64_rt_end | SALSA64_RT_END_LOOP */
	uint8_t seg;          /* SALSA64_REC_KSEG0, KSEG1 o MAPPED (rec_tlb.h) */
	uint8_t flags;        /* SALSA64_RT_F_* */
	uint8_t ins[SALSA64_RT_CLASSES];
	uint16_t whot[SALSA64_RT_CLASSES], wcold[SALSA64_RT_CLASSES];
	uint32_t target;      /* destino fijo del salto final (J, JAL, condicional), 0 si no lo tiene */
	uint8_t nbr;          /* saltos del bloque (el final y los condicionales internos que salen antes) */
};

/* Lo que pasó desde que quien llama lo puso a cero (por campo). */
struct salsa64_rec_trace_field {
	uint64_t compile_ticks;
	uint32_t compiled, retrans, words;
	uint32_t evictions, flushes, killed;
};

/* JR por bloque vivo: lo escribe exit_jr_count (rec.c), 16 bytes. */
struct salsa64_rec_trace_jr {
	uint32_t last;        /* último destino (32 bits bajos) */
	uint32_t count;       /* JR ejecutados */
	uint32_t repeat;      /* de ellos, al mismo destino que la vez anterior */
	uint32_t pad;
};

/* JR plegados por dirección del bloque. */
struct salsa64_rec_trace_jrsite {
	uint32_t vpc;         /* 0 = libre */
	uint32_t count, repeat;
	uint8_t end, n;
	uint16_t pad;
};

/* Un bloque de una ventana. */
struct salsa64_rec_trace_id {
	uint32_t vpc, code, target;
	uint16_t id, hot, cold;
	uint8_t n, end, nbr;
};

/* Marcas de C en la ventana (la entrada a un bloque es su id, < 2^16). */
#define SALSA64_RT_M_EXIT    0x81000000u  /* vuelta al despachador: | SALSA64_REC_EXIT_* (8: excepción o ERET) */
#define SALSA64_RT_M_COMPILE 0x82000000u  /* | id compilado */
#define SALSA64_RT_M_INTERP  0x83000000u  /* | instrucciones del intérprete */
#define SALSA64_RT_M_EVENT   0x84000000u  /* eventos del planificador */
#define SALSA64_RT_M_IDLE    0x85000000u  /* bucle de espera */

struct salsa64_rec_trace {
	/* Opciones: antes de salsa64_rec_init (preámbulo); después valen para
	 * los bloques que se compilen. */
	bool seq, jr;
	uint32_t win_len;     /* entradas por ventana (≤ SALSA64_RT_WIN_MAX) */
	/* Avisos (hilo que emula; pueden ser NULL). kind: 0 tramo frío, 1 mitad
	 * caliente, 2 vaciado (seg = la causa, enum salsa64_rec_flush_cause). */
	void (*on_block)(void *ctx, const struct salsa64_rec_trace_block *b);
	void (*on_evict)(void *ctx, unsigned kind, unsigned seg, unsigned killed);
	void *ctx;

	struct salsa64_rec_trace_field field;
	uint64_t compiled, retrans, words, evictions, flushes, killed;
	uint32_t seen_count, seen_full;

	/* Ventana: se abre en el despachador al llegar a arm_at (ciclo; 0 =
	 * ninguna pedida) y se llena con el código generado y las marcas de C. */
	uint64_t arm_at;
	bool armed;           /* abierta, o llena sin recoger */
	uint64_t win_cycles;  /* ciclo en que se abrió */
	uint32_t win[SALSA64_RT_WIN_MAX];
	struct salsa64_rec_trace_id tab[SALSA64_RT_TAB_MAX];
	uint32_t ntab, tab_full;
	uint8_t in_tab[SALSA64_RT_IDS / 8];

	/* Por id de bloque. */
	uint8_t end[SALSA64_RT_IDS], nbr[SALSA64_RT_IDS];
	uint16_t hot[SALSA64_RT_IDS], cold[SALSA64_RT_IDS];
	uint32_t target[SALSA64_RT_IDS];
	struct salsa64_rec_trace_jr jr_id[SALSA64_RT_IDS];
	struct salsa64_rec_trace_jrsite jr_sites[SALSA64_RT_JR_SITES];
	uint32_t jr_nsites, jr_full;
	uint64_t seen[SALSA64_RT_SEEN];

	/* Lo que deja el emisor del bloque en curso (rec_emit.c). */
	struct salsa64_rec_trace_block cur;
};

static inline uint64_t salsa64_rec_trace_now(void)
{
#if defined(__powerpc64__)
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
#else
	return 0;
#endif
}

void salsa64_rec_trace_init(struct salsa64_rec_trace *t);

/* Una marca de C en la ventana abierta (el despachador). */
static inline void salsa64_rec_trace_mark(struct salsa64_cpu *cpu, uint32_t m)
{
	if (!cpu->rec_tron)
		return;
	*cpu->rec_tpos++ = m;
	if (cpu->rec_tpos >= cpu->rec_tend)
		cpu->rec_tron = 0;
}

/* Abre la ventana (el despachador, al llegar a arm_at). */
void salsa64_rec_trace_arm(struct salsa64_rec *rec);
/* Si la ventana se llenó (o, con `force`, aunque no), la cierra: deja en
 * tab[] los bloques que aparecen (ntab) y devuelve cuántas entradas hay en
 * win[], o 0 si no había ventana o sigue abierta. Entre campos, desde el
 * hilo que emula. */
unsigned salsa64_rec_trace_take(struct salsa64_rec *rec, bool force);

/* rec.c: el bloque `id` acaba de compilarse (rec->trace->cur ya lo
 * describe): huella vista, cuentas y aviso. */
void salsa64_rec_trace_compiled(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg,
				uint32_t code, unsigned n, uint64_t ticks);
/* rec.c: el bloque `id` va a morir: sus JR pasan a la tabla por dirección. */
void salsa64_rec_trace_fold(struct salsa64_rec *rec, unsigned id);
void salsa64_rec_trace_fold_all(struct salsa64_rec *rec);
/* rec.c: un desalojo (kind 0 o 1) o un vaciado (2) con `killed` bloques. */
void salsa64_rec_trace_evicted(struct salsa64_rec *rec, unsigned kind, unsigned seg, unsigned killed);

/* Clase de una instrucción para las cuentas por clase (rec_emit.c). */
unsigned salsa64_rec_trace_class(uint32_t insn);

#endif
