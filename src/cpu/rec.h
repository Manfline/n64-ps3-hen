/*
 * Salsa64 — recompilador dinámico MIPS (VR4300) → PowerPC64 para el PPE.
 *
 * Diseño y decisiones en docs/rec-ppe.md. Resumen:
 *   - Unidad: bloque básico que empieza en una dirección de KSEG0 dentro de
 *     la RDRAM y termina en un salto (con su delay slot), en una instrucción
 *     que cambia el estado global (COP0, SYSCALL, BREAK) o a las
 *     SALSA64_REC_MAX_INSNS instrucciones. También desde KSEG1 o mapeado por
 *     la TLB, en la RDRAM o en la ROM (rec_tlb.h). Lo demás (modos
 *     usuario/supervisor, interrupciones, delay slots pendientes) lo corre el
 *     intérprete, que es la referencia.
 *   - Mismo resultado que el intérprete, instrucción por instrucción y ciclo
 *     por ciclo: un bloque solo corre en nativo si cabe entero antes del
 *     próximo evento, y después de cada llamada a C el código revisa
 *     excepciones, interrupciones y eventos y sale si hace falta.
 *   - Registros MIPS de 64 bits en registros del PPE de 64 bits (r14–r25),
 *     asignados por bloque; el resto se lee y escribe en cpu->gpr.
 *   - Enlace directo entre bloques (se parchea un `b`), invalidación por
 *     escritura con una tabla de 128 bytes por entrada (code_flags) que
 *     revisan los almacenamientos nativos y el intérprete.
 *
 * Toda la memoria va dentro de struct salsa64_rec (unos 7 MB, se reserva una
 * vez al arrancar) salvo la caché de código, que la pone quien llama: en la
 * PS3, la cola NOBITS del segmento de texto (docs/jit-ppe.md); en un host
 * ppc64 Linux, un mmap con permiso de ejecución. En otras arquitecturas
 * salsa64_rec_init devuelve false y todo corre en el intérprete.
 */
#ifndef SALSA64_CPU_REC_H
#define SALSA64_CPU_REC_H

#include "core/bus.h"
#include "core/types.h"
#include "cpu/rec_prof.h"
#include "cpu/rec_tlb.h"

struct salsa64_cpu;

#define SALSA64_REC_MAX_INSNS   64        /* instrucciones MIPS por bloque */
#define SALSA64_REC_MAX_BLOCKS  65535u    /* ids 1..65535; 0 = sin bloque */
#define SALSA64_REC_MAX_LINKS   (1u << 17)
#define SALSA64_REC_MAP_WORDS   (SALSA64_RDRAM_MAX / 4)
#define SALSA64_REC_FLAG_SHIFT  7         /* code_flags: una entrada por 128 bytes */
#define SALSA64_REC_FLAG_BYTES  (SALSA64_RDRAM_MAX >> SALSA64_REC_FLAG_SHIFT)
#define SALSA64_REC_BUF_WORDS   16384     /* búfer de compilación por sección */
#define SALSA64_REC_MAX_LABELS  4096
#define SALSA64_REC_MAX_FIXUPS  8192
#define SALSA64_REC_SEGMENTS    8         /* tramos de la caché: llena, se desaloja el más viejo (docs/rec-ppe.md) */

/* 16 bytes: la búsqueda de destinos de JR en el código generado indexa
 * blocks[] con un desplazamiento y lee `code` en +0. */
struct salsa64_rec_block {
	uint32_t code;       /* palabra de entrada en la caché; 0 = lo corre el intérprete */
	uint32_t pa;         /* dirección física de la primera instrucción */
	uint32_t links;      /* enlaces que llegan aquí: índice + 1 en links[], 0 = ninguno;
			      * en un id libre, el siguiente id libre */
	uint16_t n;          /* instrucciones del camino más largo */
	uint16_t bytes;      /* bytes de MIPS que cubre */
};

struct salsa64_rec_link {
	uint32_t site;       /* palabra parcheada (un `b` al bloque; antes, un nop) */
	uint32_t next;       /* índice + 1 del siguiente, 0 = fin */
	uint32_t gen;        /* seg_gen[] del tramo del sitio al enlazar: si cambió, el sitio es de otro bloque */
	uint32_t pad;
};

struct salsa64_rec_stats {
	uint64_t compiled;       /* bloques compilados (sin los temporales) */
	uint64_t temp_compiled;  /* bloques recortados del modo estricto */
	uint64_t interp_only;    /* direcciones que se quedaron en el intérprete */
	uint64_t entries;        /* entradas desde C al código generado */
	uint64_t linked;         /* enlaces parcheados */
	uint64_t invalidated;    /* bloques invalidados */
	uint64_t flushes;        /* vaciados completos de la caché */
	uint64_t evictions;      /* tramos desalojados (en vez de vaciar todo) */
	uint64_t interp_steps;   /* instrucciones que corrió el intérprete */
	uint64_t exec_calls;     /* instrucciones que el código generado le pasó al intérprete */
	uint64_t compiled_x;     /* de compiled, los de KSEG1 o mapeados (rec_tlb.c) */
	uint64_t rekeyed;        /* bloques mapeados enganchados en otra física con los mismos bytes (rec_tlb.c) */
	uint64_t detached;       /* bloques mapeados desprendidos al reescribirse su página (rec_tlb.c) */
	uint64_t vt_probes;      /* traducciones con la TLB del CPU (rec_tlb.c) */
	uint64_t compile_us;     /* reservado para quien mida el tiempo de compilación */
};

/* Etiquetas y correcciones del ensamblador de rec_emit.c. */
struct salsa64_rec_label {
	uint32_t pos;        /* palabra dentro de su sección, o destino absoluto */
	uint8_t sec;         /* 0 caliente, 1 fría, 2 absoluta (palabra de la caché), 3 sin fijar */
};

struct salsa64_rec_fixup {
	uint32_t at;         /* palabra que se corrige */
	uint16_t label;
	uint8_t sec;         /* sección de la palabra */
	uint8_t cond;        /* 1 = bc (14 bits), 0 = b/bl (24 bits) */
};

/* Opciones: se ponen en rec->opts antes de salsa64_rec_init, que las
 * conserva. */
#define SALSA64_REC_OPT_PIN   1u  /* sp, ra, v0, v1, a0–a3 fijos en r18–r25 entre bloques */
#define SALSA64_REC_OPT_EAGER 2u  /* escribir cada registro poco después de su último valor */
/* Para medir: conversiones y SQRT de la FPU, CFC1/CTC1, LWL/LWR y compañía
 * por el intérprete y MFC0 terminando el bloque, como antes de traducirlos
 * (docs/perf-log.md). Se aplica al compilar: cambiarla pide vaciar la caché. */
#define SALSA64_REC_OPT_RARE_EXEC 4u
/* Para medir A/B en la PS3 con el mismo binario: lo de antes del 2026-10-10
 * (noche). COP0 y ERET vuelven al despachador y el intérprete corre el resto
 * del bloque básico, CACHE al intérprete, sin camino rápido de datos
 * mapeados (ktlb solo para código), x_sync vuelve a traducir todas las
 * páginas anotadas y no prellena, exit_jr mira solo la cabeza de xhash. Se
 * aplica al compilar y en el despachador: fijarla antes de salsa64_rec_init. */
#define SALSA64_REC_OPT_LEGACY 8u
/* Para medir A/B: la FPU como hasta el 2026-10-11, cada operación por
 * cpu->fpr (rec_emit.c, emit_fpu) en vez de registros residentes y FCR31
 * perezoso (rec_emit_fpu.h). Se aplica al compilar. */
#define SALSA64_REC_OPT_FPU_MEM 16u
/* Para medir A/B: COP0 como hasta el 2026-10-11, todo por el intérprete
 * (MTC0 de EntryLo/PageMask/Index, TLBWI/TLBWR y ERET tienen ahora camino
 * propio: rec_emit_ext.h y rec_h_tlbw/rec_h_eret en rec.c). Se aplica al
 * compilar. */
#define SALSA64_REC_OPT_COP0_EXEC 32u
/* Para medir A/B: la caché llena se vacía entera, como hasta el 2026-10-11,
 * en vez de desalojar el tramo más viejo. Se aplica en el despachador. */
#define SALSA64_REC_OPT_NO_EVICT 64u
/* Depuración (host, SALSA64_REC_OPTS): apagan una pieza nueva a la vez para
 * acotar una diferencia en --lockstep. LEGACY las apaga todas. */
#define SALSA64_REC_DBG_NO_ENDLINK 0x100u   /* COP0 vuelve al despachador e interpreta el resto del bloque básico */
#define SALSA64_REC_DBG_NO_CACHE   0x200u   /* CACHE al intérprete */
#define SALSA64_REC_DBG_NO_CHAIN   0x400u   /* exit_jr mira solo la cabeza de xhash */
#define SALSA64_REC_DBG_NO_TSYNC   0x800u   /* x_sync vuelve a traducir todo y no prellena */
#define SALSA64_REC_DBG_NO_DATA    0x1000u  /* sin camino rápido de datos mapeados ni anotarlos */
#define SALSA64_REC_DBG_ERET_BOUND 0x2000u  /* tras un ERET, el intérprete hasta el fin del bloque básico */
#define SALSA64_REC_DBG_NO_DETACH  0x4000u  /* invalidar mata el bloque (sin desprender ni enganchar) */
#define SALSA64_REC_DBG_NO_SUPER   0x8000u  /* un salto condicional hacia adelante termina el bloque */
#define SALSA64_REC_DBG_NO_IJUMP   0x10000u /* un salto interno con destino dentro del bloque sale enlazado */
#define SALSA64_REC_OLD(rec, bit)  ((rec)->opts & (SALSA64_REC_OPT_LEGACY | (bit)))

struct salsa64_rec {
	uint32_t opts;
	struct salsa64_cpu *cpu;
	uint32_t *code;          /* caché de código */
	uint32_t code_words;     /* capacidad, en palabras */
	uint32_t used;           /* palabras ocupadas (asignación lineal dentro del tramo en uso) */
	uint32_t scratch;        /* inicio de la zona de bloques temporales */
	uint32_t generation;     /* sube en cada vaciado completo y en cada desalojo */
	uint32_t seg_words;      /* palabras por tramo */
	uint32_t seg_end;        /* fin del tramo en uso */
	uint32_t cur_seg;
	uint32_t seg_gen[SALSA64_REC_SEGMENTS];  /* sube al desalojar el tramo (enlaces con sitio en él) */
	uint64_t enter_opd[3];   /* descriptor ELFv1 de la entrada al código generado */
	uint32_t exit_link, exit_mid, exit_jump;  /* rutinas de salida (palabra en la caché) */
	uint32_t exit_jr;        /* JR/JALR: busca el bloque destino y salta, o sale como exit_jump */
	uint32_t exit_idle;      /* salto que el intérprete consultaría como bucle de espera */
	uint32_t exit_start;     /* el bloque salió en la entrada (no cabe antes del evento, COP1 inusable) */
	uint32_t exit_end;       /* el bloque terminó en una instrucción que vuelve a C (COP0, SYSCALL…) */
	uint32_t ktlb_rtn[2][4]; /* traducción de datos por ktlb[] (rec_tlb.h): [almacena][alineación 0, 1, 3, 7];
				  * r3 = va; vuelve con cr1 eq y r12 = desplazamiento en la RDRAM, o cr1 ne */
	uint32_t slow_load[7], slow_store[4];  /* camino lento compartido por tipo (rec_emit.c, emit_load/emit_store) */

	/* Direcciones de entrada de las funciones de C que llama el código. */
	uint64_t h_load, h_store, h_exec, h_exception, h_tlbw, h_eret;
	bool calls_near;         /* todas a menos de 32 MB del preámbulo: `bl` directo (cada bloque lo decide para sí) */

	uint32_t rdram_size;
	unsigned rdram_shift;    /* log2(rdram_size) */
	uint32_t fr;             /* Status.FR con que se compiló la caché */
	bool strict;             /* pruebas: nunca interpreta lo que se puede recompilar */
	bool no_link;            /* sin enlaces: cada bloque (o bucle de un bloque) vuelve a C */
	bool map_dirty;

	unsigned cur_id;         /* id del bloque que se compila (0 en los temporales): perfil por bloque */
	uint16_t free_block;     /* lista de ids libres */
	uint32_t next_block;     /* siguiente id nunca usado */
	uint32_t free_link;      /* lista de entradas libres de links[] (índice + 1) */
	uint32_t next_link;

	/* Depuración: si no es NULL, se llama después de cada salida del código
	 * generado (modo traza, ver src/host/run.c). */
	void (*after_exit)(struct salsa64_rec *rec, uint64_t block_pc, void *ctx);
	void *after_exit_ctx;
	/* Depuración: cada bloque compilado (palabra de entrada y tamaño). */
	void (*on_compile)(struct salsa64_rec *rec, uint64_t pc, uint32_t entry, uint32_t words, bool temp);
	/* Perfil por causa (rec_prof.h); NULL = apagado. Lo pone quien llama,
	 * antes o después de salsa64_rec_init (que lo conserva). */
	struct salsa64_rec_prof *prof;

	struct salsa64_rec_stats stats;

	/* Compilación (rec_emit.c). */
	uint32_t buf[2][SALSA64_REC_BUF_WORDS];
	uint32_t nbuf[2];
	struct salsa64_rec_label labels[SALSA64_REC_MAX_LABELS];
	struct salsa64_rec_fixup fixups[SALSA64_REC_MAX_FIXUPS];
	uint32_t nlabels, nfixups;
	bool overflow;

	uint16_t map[SALSA64_REC_MAP_WORDS];          /* bloque que empieza en cada palabra */
	struct salsa64_rec_x x;                       /* fuera de KSEG0 (rec_tlb.h): justo antes de flags */
	uint8_t flags[SALSA64_REC_FLAG_BYTES];        /* bloques que tocan cada 128 bytes (satura en 255) */
	struct salsa64_rec_block blocks[SALSA64_REC_MAX_BLOCKS + 1];
	struct salsa64_rec_link links[SALSA64_REC_MAX_LINKS];
};

/*
 * Prepara el recompilador sobre `cpu` con una caché de código de `bytes`
 * (alineada a 128 bytes, escribible y ejecutable) y lo activa
 * (cpu->rec). Devuelve false si el host no puede ejecutar el código que
 * genera (no es ppc64 big-endian) o la caché es muy chica; entonces el CPU
 * sigue con el intérprete.
 */
bool salsa64_rec_init(struct salsa64_rec *rec, struct salsa64_cpu *cpu, void *code, size_t bytes);
/* Vuelve al intérprete. */
void salsa64_rec_detach(struct salsa64_rec *rec);
/* Igual que salsa64_cpu_run, con el recompilador. */
void salsa64_rec_run(struct salsa64_rec *rec);
/* La RDRAM [pa, pa + len) cambió fuera del CPU (DMA, carga de la ROM,
 * pruebas): descarta el código que dependa de ella. */
void salsa64_rec_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t len);
/* Descarta todo el código generado. */
void salsa64_rec_flush(struct salsa64_rec *rec);
/* Palabras de la caché ocupadas. */
static inline uint32_t salsa64_rec_used(const struct salsa64_rec *rec) { return rec->used; }

/* ---- Interno: compartido por rec.c y rec_emit.c ---------------------------- */

/* Cómo volvió el código generado a C (cpu->rec_kind). */
enum {
	SALSA64_REC_EXIT_LINK = 1,  /* a una dirección fija; cpu->rec_site es el enlace a parchear */
	SALSA64_REC_EXIT_MID = 2,   /* a mitad de bloque (evento, interrupción, código invalidado) */
	SALSA64_REC_EXIT_JUMP = 3,  /* a un destino calculado (JR/JALR) o sin enlace */
	SALSA64_REC_EXIT_IDLE = 4,  /* r3 = pc de un salto tomado, antes de su delay slot (idle.c) */
	SALSA64_REC_EXIT_START = 5, /* como EXIT_MID, en la entrada del bloque: no cabe antes del evento o COP1 inusable */
	SALSA64_REC_EXIT_END = 6,   /* el bloque terminó completo en una instrucción de K_END; sigue en r3 */
};

/* Tipos de acceso del camino lento (salsa64_rec_h_load/h_store). */
enum { SALSA64_REC_LB, SALSA64_REC_LBU, SALSA64_REC_LH, SALSA64_REC_LHU,
       SALSA64_REC_LW, SALSA64_REC_LWU, SALSA64_REC_LD };
enum { SALSA64_REC_SB, SALSA64_REC_SH, SALSA64_REC_SW, SALSA64_REC_SD };

/* Con SALSA64_REC_OPT_PIN: el registro MIPS salsa64_rec_pin[k] vive en
 * r(SALSA64_REC_PIN_PPC + k) mientras corre el código generado; la entrada lo
 * carga y la salida a C lo escribe. Son los de la convención de llamadas:
 * pasan de un bloque a otro en cada llamada y cada retorno. */
#define SALSA64_REC_PIN_N   8
#define SALSA64_REC_PIN_PPC 18
static const uint8_t salsa64_rec_pin[SALSA64_REC_PIN_N] __attribute__((unused)) = { 29, 31, 2, 3, 4, 5, 6, 7 };

/* rec.c (interno): deja visible para el PPE el código recién escrito. */
void salsa64_rec_sync(const void *p, size_t bytes);
/* rec.c (interno): ¿un bloque que empieza en la palabra `base` llama a las
 * funciones de C con `bl` (a menos de 32 MB)? Si no, por mtctr/bctrl. */
bool salsa64_rec_calls_near_at(const struct salsa64_rec *rec, uint32_t base);

/* Constantes de la FPU residente (rec_emit_fpu.h): la entrada al código
 * generado las deja en f26–f31, en este orden: 1,0; 0,0; y los bordes de la
 * franja segura de fpu_fast_band en S (2^-60, 2^61) y en D (2^-440, 2^441). */
extern const double salsa64_rec_fconst[6];

/* rec_emit.c (interno): compila el bloque en `pc` (física `pa`, segmento
 * `seg` de rec_tlb.h) de hasta `max` instrucciones. Devuelve la palabra de
 * entrada en la caché, o 0 si no se pudo; `n` recibe las instrucciones del
 * camino más largo y `bytes` las que cubre. */
uint32_t salsa64_rec_compile(struct salsa64_rec *rec, uint64_t pc, uint32_t pa, unsigned seg, unsigned max,
			     bool temp, unsigned *n, uint32_t *bytes);

#endif
