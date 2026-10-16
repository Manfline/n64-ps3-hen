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

/* Causas contadas solo cuando la sesión activa llama a una rutina lenta. */
enum salsa64_rt_slow {
	SALSA64_RT_SLOW_KSEG0, SALSA64_RT_SLOW_KSEG1,
	SALSA64_RT_SLOW_TLB_RDRAM, SALSA64_RT_SLOW_TLB_IO,
	SALSA64_RT_SLOW_TLB_EXCEPTION, SALSA64_RT_SLOW_MISALIGNED,
	SALSA64_RT_SLOW_MMIO_SP, SALSA64_RT_SLOW_MMIO_DP, SALSA64_RT_SLOW_MMIO_MI,
	SALSA64_RT_SLOW_MMIO_VI, SALSA64_RT_SLOW_MMIO_AI, SALSA64_RT_SLOW_MMIO_PI,
	SALSA64_RT_SLOW_MMIO_RI, SALSA64_RT_SLOW_MMIO_SI, SALSA64_RT_SLOW_MMIO_PIF,
	SALSA64_RT_SLOW_MMIO_CART, SALSA64_RT_SLOW_CODE_STORE,
	SALSA64_RT_SLOW_EXEC_COP0, SALSA64_RT_SLOW_EXEC_FPU, SALSA64_RT_SLOW_EXEC_CACHE,
	SALSA64_RT_SLOW_EXEC_LLSC, SALSA64_RT_SLOW_EXEC_TRAP, SALSA64_RT_SLOW_EXEC_OTHER,
	SALSA64_RT_SLOW_EXCEPTION,
	SALSA64_RT_SLOW_EXC_INT, SALSA64_RT_SLOW_EXC_MOD, SALSA64_RT_SLOW_EXC_TLBL,
	SALSA64_RT_SLOW_EXC_TLBS, SALSA64_RT_SLOW_EXC_ADEL, SALSA64_RT_SLOW_EXC_ADES,
	SALSA64_RT_SLOW_EXC_IBE, SALSA64_RT_SLOW_EXC_DBE, SALSA64_RT_SLOW_EXC_SYS,
	SALSA64_RT_SLOW_EXC_BP, SALSA64_RT_SLOW_EXC_RI, SALSA64_RT_SLOW_EXC_CPU,
	SALSA64_RT_SLOW_EXC_OV, SALSA64_RT_SLOW_EXC_TR, SALSA64_RT_SLOW_EXC_FPE,
	SALSA64_RT_SLOW_EXC_WATCH, SALSA64_RT_SLOW_N
};

#define SALSA64_RT_TOP_PC 16u
struct salsa64_rec_trace_slow_pc { uint32_t pc; uint64_t ticks; uint32_t calls; };
/* Las cuentas por causa son totales de la sesión (la ventana es la resta).
 * Los PCs no se pueden restar: una entrada de la tabla (Space-Saving) puede
 * pasar a otro PC y volver con menos de lo que tenía. Por eso `slow.top` es
 * de toda la sesión y `slow_field.top` de la ventana en curso: quien la
 * escribe la copia y pide con `top_reset` que el hilo que emula la vacíe
 * antes de la próxima cuenta. */
struct salsa64_rec_trace_slow_stats {
	uint64_t ticks[SALSA64_RT_SLOW_N];
	uint32_t calls[SALSA64_RT_SLOW_N];
	struct salsa64_rec_trace_slow_pc top[SALSA64_RT_TOP_PC];
};

/* Desglose de lo que llega a C por COP0 (rec_h_exec, rec_h_tlbw y
 * rec_h_eret) y por los registros del MI (rec_h_load y rec_h_store): por
 * instrucción y registro de COP0 (MFC0, MTC0, DMFC0 y DMTC0 × 32), las de
 * la TLB, ERET y lo demás; y por registro del MI, lectura o escritura. */
enum salsa64_rt_cop0 {
	SALSA64_RT_COP0_MF = 0, SALSA64_RT_COP0_MT = 32, SALSA64_RT_COP0_DMF = 64, SALSA64_RT_COP0_DMT = 96,
	SALSA64_RT_COP0_TLBR = 128, SALSA64_RT_COP0_TLBWI, SALSA64_RT_COP0_TLBWR, SALSA64_RT_COP0_TLBP,
	SALSA64_RT_COP0_ERET, SALSA64_RT_COP0_OTHER,
	SALSA64_RT_COP0_MI,                       /* + 2 × registro (0 MODE, 1 VERSION, 2 INTR, 3 INTR_MASK) + escritura */
	SALSA64_RT_COP0_N = SALSA64_RT_COP0_MI + 8
};
struct salsa64_rec_trace_cop0 {
	uint64_t ticks[SALSA64_RT_COP0_N];
	uint32_t calls[SALSA64_RT_COP0_N];
};
/* Lo que escribió la ventana anterior, para restar (perfilado.c). */
struct salsa64_rec_trace_prev {
	struct salsa64_rec_trace_slow_stats slow;
	struct salsa64_rec_trace_cop0 cop0;
};

/* Desglose de las instrucciones de COP1 que caen al intérprete: por (fmt,
 * funct) para S, D, W y L, más las que no son aritméticas, y la razón según
 * FCR31 antes de ejecutarla. Preasignado y acotado. */
enum salsa64_rt_fpu_reason {
	SALSA64_RT_FPU_R_RM,      /* modo de redondeo distinto de cero */
	SALSA64_RT_FPU_R_INEXACT, /* causa I (inexacto) habilitada */
	SALSA64_RT_FPU_R_ENABLED, /* otra excepción habilitada en FCR31 */
	SALSA64_RT_FPU_R_RESULT,  /* el resto: resultado o instrucción que el nativo no cubre */
	SALSA64_RT_FPU_R_N
};
enum salsa64_rt_fpu_extra {
	SALSA64_RT_FPU_MFC1, SALSA64_RT_FPU_DMFC1, SALSA64_RT_FPU_CFC1, SALSA64_RT_FPU_MTC1,
	SALSA64_RT_FPU_DMTC1, SALSA64_RT_FPU_CTC1, SALSA64_RT_FPU_BC1, SALSA64_RT_FPU_OTHER,
	SALSA64_RT_FPU_EXTRA_N
};
#define SALSA64_RT_FPU_ARITH   256u   /* 4 formatos (S, D, W, L) × 64 funct */
#define SALSA64_RT_FPU_N       (SALSA64_RT_FPU_ARITH + SALSA64_RT_FPU_EXTRA_N)
struct salsa64_rec_trace_fpu {
	uint64_t ticks[SALSA64_RT_FPU_N];
	uint32_t calls[SALSA64_RT_FPU_N][SALSA64_RT_FPU_R_N];
	uint32_t fr_calls[2];     /* con Status.FR = 0 y 1 */
	uint64_t fr_ticks[2];
};

enum salsa64_rt_compile_reason {
	SALSA64_RT_COMPILE_NEW, SALSA64_RT_COMPILE_CPU_WRITE, SALSA64_RT_COMPILE_DMA,
	SALSA64_RT_COMPILE_EVICT, SALSA64_RT_COMPILE_RETRANS, SALSA64_RT_COMPILE_N
};
struct salsa64_rec_trace_compile_stats {
	uint64_t ticks[SALSA64_RT_COMPILE_N];
	uint32_t calls[SALSA64_RT_COMPILE_N], words[SALSA64_RT_COMPILE_N];
	uint32_t max_field_compiles, max_field;
};
struct salsa64_rec_trace_inval { uint32_t lo, hi, remaining; uint8_t reason; };

/* Dirección virtual y opcodes: funciones puras para el lector y las pruebas. */
unsigned salsa64_rec_trace_mem_class(uint64_t va, uint32_t pa, bool ok, bool store, bool aligned,
				     uint32_t rdram_size);
unsigned salsa64_rec_trace_exec_class(uint32_t instr);
unsigned salsa64_rec_trace_exception_class(unsigned exception);
/* Índice en salsa64_rec_trace_fpu (arit.: fmt*64 + funct, fmt 0..3 = S, D, W,
 * L; después, 256 + salsa64_rt_fpu_extra) y razón según FCR31 previo. */
unsigned salsa64_rec_trace_fpu_index(uint32_t instr);
unsigned salsa64_rec_trace_fpu_reason(uint32_t fcr31);
/* Índice en salsa64_rec_trace_cop0 de una instrucción de COP0, y de un
 * acceso a un registro del MI (dirección física). */
unsigned salsa64_rec_trace_cop0_index(uint32_t instr);
unsigned salsa64_rec_trace_mi_index(uint32_t pa, bool store);

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
	uint8_t reason;       /* enum salsa64_rt_compile_reason */
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
	struct salsa64_rec_trace_slow_stats slow, slow_field;
	struct salsa64_rec_trace_fpu fpu;  /* total de sesión */
	struct salsa64_rec_trace_cop0 cop0;  /* total de sesión */
	volatile uint8_t top_reset;          /* vaciar slow_field.top (lo pide quien escribe la ventana) */
	struct salsa64_rec_trace_compile_stats compiles, compiles_field;
	struct salsa64_rec_trace_inval inval[32];
	uint8_t inval_next;
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
void salsa64_rec_trace_slow_add(struct salsa64_rec_trace *t, unsigned cause, uint64_t ticks, uint32_t pc);
/* Una instrucción de COP1 que corrió el intérprete (fcr31 y status: el estado
 * antes de ejecutarla). */
void salsa64_rec_trace_fpu_add(struct salsa64_rec_trace *t, uint32_t instr, uint32_t fcr31, uint32_t status,
			       uint64_t ticks);
void salsa64_rec_trace_invalidate(struct salsa64_rec_trace *t, unsigned reason, uint32_t pa, uint32_t len,
				  uint32_t killed);
/* Una entrada del desglose de COP0 y el MI (índice de las dos de arriba). */
static inline void salsa64_rec_trace_cop0_add(struct salsa64_rec_trace *t, unsigned k, uint64_t ticks)
{
	if (k < SALSA64_RT_COP0_N) {
		t->cop0.ticks[k] += ticks;
		t->cop0.calls[k]++;
	}
}

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
