/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — muestreo por tiempo del recompilador: dónde está el hilo de
 * emulación cada tanto (docs/rec-ppe.md, «Muestreo por tiempo»).
 *
 * El perfil persistente (rec_profile.h) cuenta entradas a bloques, y eso
 * engaña: los bloques más entrados son los de 2–4 instrucciones. Lo que
 * cuesta es tiempo, y sin el PMU del Cell (privilegiado) la forma de medirlo
 * es muestrear. El hilo que emula deja dos bytes en `struct salsa64_cpu`:
 *   - rec_cur: el id del bloque que corre (lo escribe la entrada de cada
 *     bloque, dos palabras: `li` y `sth`, solo con el muestreo puesto al
 *     compilar; los bucles no vuelven a pasar por ahí);
 *   - rec_mark: 0 en código generado, o la categoría de lo que hace fuera
 *     de él (la rutina compartida del camino lento al llamar a C, el
 *     despachador, el intérprete, los eventos, compilar, los bucles de
 *     espera, el ritmo).
 * Otro hilo (en la PS3, uno de baja prioridad en el otro hilo de hardware
 * del PPE; cuesta ~0,13 ms por ms al hilo que emula, y una muestra son
 * decenas de ciclos) lee los dos bytes cada ~200 µs y suma una muestra al
 * bloque o a la categoría. Al morir un bloque (o al informar) sus muestras
 * pasan a una tabla por dirección, así que un bloque recompilado varias
 * veces suma todo bajo su dirección.
 *
 * Lo que no puede dar: en qué parte del cuerpo de un bloque está el hilo
 * (desde usuario no se lee el pc nativo del otro hilo). Sí distingue el
 * cuerpo del bloque, los caminos lentos en C y el resto del emulador.
 *
 * Carreras: el hilo que muestrea incrementa contadores que el que emula
 * lee y pone a cero al plegar; una muestra se puede perder en la ventana.
 * Es estadística, no cuenta exacta.
 */
#ifndef SALSA64_CPU_REC_SAMPLE_H
#define SALSA64_CPU_REC_SAMPLE_H

#include "core/types.h"
#include "cpu/vr4300.h"

struct salsa64_rec;

#define SALSA64_SMP_IDS   65536u   /* ids de bloque (rec.h) */
#define SALSA64_SMP_TABLE 16384u   /* entradas por dirección */

/* cpu->rec_mark: 0 = en código generado (cpu->rec_cur dice el bloque). */
enum salsa64_smp_mark {
	SALSA64_SMP_NATIVE = 0,
	SALSA64_SMP_SLOW,      /* la rutina compartida del camino lento, en C (cargas y almacenamientos fuera del
				* camino rápido, una instrucción al intérprete, una excepción) */
	SALSA64_SMP_DISPATCH,  /* el despachador: buscar el bloque, enlazar, tras una salida */
	SALSA64_SMP_INTERP,    /* el intérprete (lo que el recompilador no toma) */
	SALSA64_SMP_EVENTS,    /* eventos del planificador (VI, RSP, DP, AI, Compare…) */
	SALSA64_SMP_COMPILE,   /* compilar un bloque */
	SALSA64_SMP_IDLE,      /* bucles de espera (idle.c) */
	SALSA64_SMP_PACE,      /* el hilo que emula duerme al ritmo de la tele emulada */
	SALSA64_SMP_OTHER,     /* lo demás del hilo que emula fuera del CPU (mandos, guardado, colas) */
	SALSA64_SMP_N
};

struct salsa64_rec_sample_entry {
	uint32_t pc;         /* dirección virtual del bloque (32 bits); 0 = libre */
	uint32_t samples;
	uint16_t n;          /* instrucciones MIPS */
	uint16_t words;      /* palabras de PPC (del último bloque compilado ahí) */
	uint16_t hot;        /* de ellas, la sección caliente */
	uint16_t pad;
};

struct salsa64_rec_sample {
	uint64_t ticks;                          /* muestras tomadas */
	uint32_t marks[SALSA64_SMP_N];           /* por categoría ([NATIVE] no se usa: es samples[] + unknown) */
	uint32_t unknown;                        /* en código generado sin id (bloques recortados del modo estricto) */
	uint32_t samples[SALSA64_SMP_IDS];       /* por id de bloque, desde el último plegado */
	uint16_t words[SALSA64_SMP_IDS];         /* palabras de PPC de cada bloque vivo */
	uint16_t hot[SALSA64_SMP_IDS];
	uint32_t count, full;                    /* entradas de la tabla; muestras sin lugar */
	struct salsa64_rec_sample_entry table[SALSA64_SMP_TABLE];  /* por dirección: índice abierto */
};

/* Lo que muestra cada ventana de estadísticas: una copia de los totales
 * para restar (salsa64_rec_sample_split). */
struct salsa64_rec_sample_prev {
	uint64_t ticks;
	uint32_t marks[SALSA64_SMP_N];
	uint32_t unknown;
};

void salsa64_rec_sample_init(struct salsa64_rec_sample *s);

/* Una muestra (el hilo que muestrea). */
static inline void salsa64_rec_sample_tick(struct salsa64_rec_sample *s, const struct salsa64_cpu *cpu)
{
	unsigned mark = cpu->rec_mark, id = cpu->rec_cur;
	s->ticks++;
	if (mark) {
		s->marks[mark < SALSA64_SMP_N ? mark : SALSA64_SMP_OTHER]++;
		return;
	}
	if (id)
		s->samples[id]++;
	else
		s->unknown++;
}

/* El bloque `id` acaba de compilarse con `words` palabras (`hot` calientes). */
void salsa64_rec_sample_compiled(struct salsa64_rec_sample *s, unsigned id, uint32_t words, uint32_t hot);
/* Pasa las muestras del bloque `id` a la tabla por dirección (el hilo que
 * emula: antes de matar el bloque, y al informar). */
void salsa64_rec_sample_fold(struct salsa64_rec *rec, unsigned id);
void salsa64_rec_sample_fold_all(struct salsa64_rec *rec);
/* Reparto por categoría desde `prev` (que se actualiza), en texto. */
void salsa64_rec_sample_split(const struct salsa64_rec_sample *s, struct salsa64_rec_sample_prev *prev, char *text,
			      size_t size);
/* Los `top` bloques con más muestras (plegando los vivos antes) y el
 * reparto total, una línea por `line`. */
void salsa64_rec_sample_report(struct salsa64_rec *rec, unsigned top, void (*line)(void *ctx, const char *text),
			       void *ctx);

#endif
