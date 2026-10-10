/*
 * Salsa64 — recompilador dinámico MIPS (VR4300) → PowerPC64 para el PPE.
 *
 * Diseño y decisiones en docs/rec-ppe.md. Resumen:
 *   - Unidad: bloque básico que empieza en una dirección de KSEG0 dentro de
 *     la RDRAM y termina en un salto (con su delay slot), en una instrucción
 *     que cambia el estado global (COP0, SYSCALL, BREAK) o a las
 *     SALSA64_REC_MAX_INSNS instrucciones. Lo demás (TLB, KSEG1, modos
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
};

struct salsa64_rec_stats {
	uint64_t compiled;       /* bloques compilados (sin los temporales) */
	uint64_t temp_compiled;  /* bloques recortados del modo estricto */
	uint64_t interp_only;    /* direcciones que se quedaron en el intérprete */
	uint64_t entries;        /* entradas desde C al código generado */
	uint64_t linked;         /* enlaces parcheados */
	uint64_t invalidated;    /* bloques invalidados */
	uint64_t flushes;        /* vaciados completos de la caché */
	uint64_t interp_steps;   /* instrucciones que corrió el intérprete */
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
#define SALSA64_REC_OPT_PIN 1u  /* sp, ra, v0, v1, a0–a3 fijos en r18–r25 entre bloques */

struct salsa64_rec {
	uint32_t opts;
	struct salsa64_cpu *cpu;
	uint32_t *code;          /* caché de código */
	uint32_t code_words;     /* capacidad, en palabras */
	uint32_t used;           /* palabras ocupadas (asignación lineal) */
	uint32_t scratch;        /* inicio de la zona de bloques temporales */
	uint32_t generation;     /* sube en cada vaciado completo */
	uint64_t enter_opd[3];   /* descriptor ELFv1 de la entrada al código generado */
	uint32_t exit_link, exit_mid, exit_jump;  /* rutinas de salida (palabra en la caché) */
	uint32_t exit_jr;        /* JR/JALR: busca el bloque destino y salta, o sale como exit_jump */

	/* Direcciones de entrada de las funciones de C que llama el código. */
	uint64_t h_load, h_store, h_exec, h_exception;
	bool calls_near;         /* todas a menos de 32 MB de la caché: `bl` directo */

	uint32_t rdram_size;
	unsigned rdram_shift;    /* log2(rdram_size) */
	uint32_t fr;             /* Status.FR con que se compiló la caché */
	bool strict;             /* pruebas: nunca interpreta lo que se puede recompilar */
	bool no_link;            /* sin enlaces ni bucles internos: cada bloque vuelve a C */
	bool map_dirty;

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

	struct salsa64_rec_stats stats;

	/* Compilación (rec_emit.c). */
	uint32_t buf[2][SALSA64_REC_BUF_WORDS];
	uint32_t nbuf[2];
	struct salsa64_rec_label labels[SALSA64_REC_MAX_LABELS];
	struct salsa64_rec_fixup fixups[SALSA64_REC_MAX_FIXUPS];
	uint32_t nlabels, nfixups;
	bool overflow;

	uint16_t map[SALSA64_REC_MAP_WORDS];          /* bloque que empieza en cada palabra */
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

/* rec_emit.c (interno): compila el bloque en `pc` de hasta `max` instrucciones.
 * Devuelve la palabra de entrada en la caché, o 0 si no se pudo; `n` recibe
 * las instrucciones del camino más largo y `bytes` las que cubre. */
uint32_t salsa64_rec_compile(struct salsa64_rec *rec, uint64_t pc, unsigned max, bool temp,
			     unsigned *n, uint32_t *bytes);

#endif
