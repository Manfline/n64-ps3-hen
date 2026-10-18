/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — censo gráfico (solo host): `salsa64-run --lle=all --lle-check-gfx`.
 *
 * Cada tarea gráfica corre en el microcódigo real (LLE: el RDP recibe sus
 * comandos por DPC_START/DPC_END y los interpreta dpc.c) y, desde el mismo
 * estado, en la HLE (otro gfx_state, otros búferes de salida). Los dos
 * resultados pasan por el mismo rasterizador de referencia (raster.c) desde
 * el mismo estado de las imágenes, y se comparan los píxeles que dejó cada
 * uno. Ver docs/gfx-censo.md.
 */
#ifndef SALSA64_HOST_GFXCHECK_H
#define SALSA64_HOST_GFXCHECK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/rcp.h"
#include "rsp/gfx/gfx.h"

/* Tolerancias de la clasificación (documentadas en docs/gfx-censo.md). */
#define GCHK_MINOR_CHANNEL 8u   /* un paso de 5 bits (raster_q5): redondeo del combiner o del sombreado */
#define GCHK_BLUR_TOL 16u  /* ni la media de 5 × 5 de los dos lados se parece a menos de esto */
#define GCHK_BIG_BAD_PPM 30000u  /* una distinta es grande si pasa del 3 % del área */
#define GCHK_MINOR_BAD_PPM 2000u  /* o a lo más 0,2 % del área de los destinos comparados con diferencias sin explicar (bordes, textura corrida) */

enum gchk_class {
	GCHK_SAME,   /* sin un solo píxel distinto */
	GCHK_MINOR,  /* diferencia menor: ±1 paso de 5 bits, o solo bordes */
	GCHK_DIFF,   /* distinta */
	GCHK_NOHLE,  /* sin HLE (solo el microcódigo real la hace) */
	GCHK_NCLASS
};

/* Qué falla en una tarea distinta (el tipo dominante). */
enum gchk_kind {
	GCHK_KIND_NONE,
	GCHK_KIND_MISSING,   /* la HLE no dibuja píxeles que dibuja el microcódigo */
	GCHK_KIND_EXTRA,     /* la HLE dibuja píxeles que el microcódigo no */
	GCHK_KIND_COLOR,     /* los dos dibujan los mismos píxeles con otro color */
	GCHK_NKIND
};

/* Resultado de comparar imágenes (se acumula sobre varios destinos). */
struct gchk_px {
	uint32_t drawn_l, drawn_h;  /* píxeles distintos del estado previo, en el LLE y en la HLE */
	uint32_t area;              /* píxeles de los destinos comparados */
	uint32_t ndiff;             /* píxeles distintos entre los dos */
	uint32_t nbad;              /* de ellos, los que no se explican por un corrimiento de un píxel, por ±8 ni por la media de 5 × 5 */
	uint32_t only_l, only_h;    /* de ellos, dibujados solo por uno */
	uint32_t both;              /* dibujados por los dos con otro color */
	uint32_t maxdiff;           /* mayor diferencia de un canal (0 a 255) */
	uint32_t hist[5];           /* píxeles distintos por diferencia: ≤8, ≤16, ≤32, ≤64, más */
	int x0, y0, x1, y1;         /* caja de lo distinto (x1, y1 exclusivos); x0 > x1 si no hay */
	uint32_t addr;              /* dirección del destino con más diferencias */
	uint32_t addr_diff;         /* sus píxeles distintos */
	/* Por destino (para depurar: SALSA64_GCHK_VERBOSE). */
	unsigned ntgt;
	struct { uint32_t addr, w, lh, ndiff, drawn_l, drawn_h; } tgt[16];
};

void gchk_px_init(struct gchk_px *p);
/* Compara w × h píxeles (0xRRGGBB, `stride` de paso) de un destino: `pre`
 * antes de la tarea, `l` tras el LLE, `h` tras la HLE. */
void gchk_px_add(struct gchk_px *acc, const uint32_t *pre, const uint32_t *l, const uint32_t *h, unsigned w,
		 unsigned hgt, unsigned stride, uint32_t addr);
enum gchk_class gchk_classify(const struct gchk_px *p);
/* Una tarea distinta con diferencias sin explicar en más del 3 % del área. */
bool gchk_is_big(const struct gchk_px *p);
enum gchk_kind gchk_kind_of(const struct gchk_px *p);
const char *gchk_class_name(enum gchk_class c);
const char *gchk_kind_name(enum gchk_kind k);

/* Dibuja `job`/`res` sobre el estado `pre` del rasterizador (raster_state_save
 * de antes de la tarea) y compara las imágenes con las de `post` (el estado
 * tras el LLE). Al volver, el rasterizador queda en `post`. Los búferes
 * están en el tamaño de raster_state_size(). */
void gchk_compare_raster(const void *pre, const void *post, const struct gfx_job *job, const struct gfx_result *res,
			 struct gchk_px *out);

/* Diferencias del estado del RDP (registros que persisten entre tareas)
 * entre dos gfx_state: una máscara de bits sobre gchk_state_field_name(). */
struct gfx_state;
unsigned gchk_state_nfields(void);
const char *gchk_state_field_name(unsigned i);
uint64_t gchk_state_diff(const struct gfx_state *a, const struct gfx_state *b);

/* --- Conducción desde salsa64-run ------------------------------------------ */

/* `lle_state` es el gfx_state que consume los comandos del RDP del LLE;
 * `field` el contador de campos del VI (para anotar la escena) y `lle_tasks`
 * los trabajos que ya terminó quien consume al LLE (numera la tarea de la HLE). */
void gchk_init(const struct salsa64_bus *bus, struct gfx_state *lle_state, const uint32_t *field,
	       const uint32_t *lle_tasks);
/* Una tarea gráfica empieza (antes de que arranque el LLE). `key` es el
 * microcódigo ("F3DEX2: RSP Gfx ucode F3DEX2 …"). */
void gchk_task_start(const struct salsa64_task *t, enum salsa64_gbi gbi, const char *key);
/* El LLE entregó un trabajo (tramo del RDP) al rasterizador. */
void gchk_lle_job(const struct gfx_job *job, const struct gfx_result *res);
/* La tarea terminó: comparar. */
void gchk_task_end(void);
bool gchk_pending(void);
/* Resumen al final (y, con SALSA64_GCHK_LOG, el detalle por tarea). */
void gchk_report(FILE *f);
/* Totales (para pruebas). */
uint32_t gchk_total_tasks(void);
uint32_t gchk_total_class(enum gchk_class c);

#endif
