/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — interpolación de cuadros por geometría: emparejamiento de los
 * vértices de un cuadro del juego con los del anterior (docs/interpolacion.md,
 * «Paso 2»). Portable (PPE y host, sin rsxkit), como targets.c.
 *
 * Con GFX_JOB_INTERP la HLE deja un registro GFX_OP_VGROUP por cada G_VTX
 * (gfx_vgroup: dirección, n y paso en la lista del juego, viewport y las
 * coordenadas de recorte de sus vértices) y en el pad de cada vértice de
 * salida su grupo y lugar (GFX_VGID; los del recorte cercano, el grupo y sus
 * dos extremos, GFX_VCLIP_ID). Aquí:
 *
 *   vmatch_build  arma la tabla de un cuadro desde sus registros: identidad
 *                 de cada grupo = dirección y cuántas veces se cargó antes esa
 *                 dirección en el cuadro (el mismo modelo dibujado varias
 *                 veces se distingue por orden); de respaldo, el contenido de
 *                 la lista en la RDRAM y sus repeticiones (Perfect Dark copia
 *                 los vértices a un búfer distinto en cada cuadro)
 *   vmatch_pair   busca cada grupo en la tabla del cuadro anterior: todo el
 *                 grupo o nada (así una malla no se rasga), sin pareja si los
 *                 vértices cerca de la pantalla se movieron más que max_disp
 *                 píxeles (corte, teletransporte), y el cuadro entero sin
 *                 interpolar si más de un tercio de los grupos con pareja se
 *                 movió así o si menos de la mitad de los vértices tiene pareja
 *                 (cambio de plano)
 *   vmatch_prev   por cada vértice de salida, su posición en el cuadro
 *                 anterior (x, y, w desde las coordenadas de recorte de allá
 *                 con el viewport del grupo; la profundidad z/w, la de ahora;
 *                 los del recorte cercano, entre las de sus dos extremos con la
 *                 misma t); sin pareja, la de ahora
 *
 * Memoria: la pone quien llama (vmatch_table_init con sus capacidades);
 * nada se reserva en el bucle.
 */
#ifndef SALSA64_RDP_VMATCH_H
#define SALSA64_RDP_VMATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

struct vmatch_group {
	uint32_t addr, hash;   /* dirección en la lista del juego y hash del contenido */
	uint32_t occ, hocc;    /* repeticiones antes en el cuadro: por dirección y por contenido */
	uint32_t n, v0, stride;
	uint32_t clip;         /* primer vértice en vmatch_table.clip */
	float vp[4];           /* vscale x, y y vtrans x, y */
	int32_t pair;          /* grupo del cuadro anterior (vmatch_pair), o -1 */
};

struct vmatch_table {
	struct vmatch_group *g;
	float (*clip)[4];
	uint32_t *index;       /* potencia de 2: posición de un grupo + 1 por clave; 0 libre */
	uint32_t gcap, ccap, icap;
	uint32_t ngroups, nclip;
	bool full;             /* no cupieron todos los registros: el cuadro no se interpola */
};

struct vmatch_stats {
	uint32_t groups, vertices;       /* del cuadro */
	uint32_t found, found_vertices;  /* con la misma identidad en el anterior */
	uint32_t paired, paired_vertices;
	uint32_t moved;                  /* con pareja pero rechazados por el desplazamiento */
	uint32_t by_content;             /* encontrados por el contenido (no por la dirección) */
	bool cut;                        /* el cuadro no se interpola */
};

/* `g` con gcap grupos, `clip` con ccap vértices e `index` con icap
 * entradas (potencia de 2, al menos 2 × gcap). */
void vmatch_table_init(struct vmatch_table *t, struct vmatch_group *g, uint32_t gcap, float (*clip)[4], uint32_t ccap,
		       uint32_t *index, uint32_t icap);
/* La tabla del cuadro desde los comandos de su tarea (cmd_bytes bytes). El
 * hash del contenido de cada grupo sale de la RDRAM (dirección, n y paso del
 * registro), mirada poco después de la tarea: la lista del cuadro sigue ahí
 * (los juegos alternan sus búferes). Sin `rdram`, todos los hash valen 0. */
void vmatch_build(struct vmatch_table *t, const uint8_t *cmd, uint32_t cmd_bytes, const uint8_t *rdram,
		  uint32_t rdram_size);
/* Empareja `cur` con `prev` (NULL: ninguno) y deja en cur->g[].pair el
 * resultado. max_disp en píxeles de la N64 por cuadro del juego. */
void vmatch_pair(const struct vmatch_table *prev, struct vmatch_table *cur, float max_disp, struct vmatch_stats *st);
/* Las posiciones en el cuadro anterior de los nvtx vértices de salida de la
 * tarea (mismo formato que gfx_vertex: x, y, z, w de salida), en `out`
 * (nvtx × 4 floats); devuelve cuántos tienen pareja. */
uint32_t vmatch_prev(const struct vmatch_table *prev, const struct vmatch_table *cur, const struct vmatch_stats *st,
		     const struct gfx_vertex *vtx, uint32_t nvtx, float (*out)[4]);

#endif
