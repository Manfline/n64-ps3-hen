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
 *   vmatch_build  arma la tabla de un cuadro desde sus registros: la clave de
 *                 cada grupo es su dirección y, de respaldo, el hash de su
 *                 contenido que deja la HLE (Perfect Dark copia los vértices a
 *                 un búfer distinto en cada cuadro, y el HUD de Zelda va en un
 *                 búfer dinámico); los grupos de una misma clave
 *                 quedan juntos y en orden (el mismo modelo dibujado varias
 *                 veces), con su centro y su tamaño en pantalla
 *   vmatch_pair   busca cada grupo entre los del cuadro anterior con su clave:
 *                 el que le toca por orden y, si hay varios, el más cercano en
 *                 pantalla de los que están cerca en el orden (instancias que
 *                 cambian de orden, partículas); todo el grupo o nada (así una
 *                 malla no se rasga). Sin pareja: si los vértices cerca de la
 *                 pantalla se movieron más que max_disp píxeles (corte,
 *                 teletransporte), si una instancia de un modelo repetido se
 *                 movió mucho más que sus hermanas (una partícula que renace
 *                 en otro lugar), si un grupo chico en pantalla se deformó de
 *                 golpe (estelas en anillo) o si a media interpolación se
 *                 aplastaría (un giro de más de un cuarto de vuelta en un
 *                 cuadro); y nunca lo que va con proyección
 *                 ortográfica (w = 1 en todos sus vértices: el HUD 3D, como
 *                 el botón A que gira en Zelda), que sale como en el cuadro de
 *                 ahora, igual que los rectángulos. Y el cuadro
 *                 entero sin interpolar si más de un tercio de los grupos con
 *                 pareja se movió así o si menos de la mitad de los vértices
 *                 tiene pareja (cambio de plano)
 *   vmatch_atomic nunca un objeto a medias (ver abajo)
 *   vmatch_prev   por cada vértice de salida, su posición en el cuadro
 *                 anterior (x, y, z, w desde las coordenadas de recorte de
 *                 allá con la vista del grupo; los del recorte cercano, entre
 *                 las de sus dos extremos con la misma t); sin pareja, la de
 *                 ahora. Quien dibuja interpola las cuatro: el mundo quieto
 *                 sale como con una sola matriz intermedia, y dos superficies
 *                 casi a la misma profundidad (el agua y la orilla) no se
 *                 cruzan
 *
 * Memoria: la pone quien llama (vmatch_table_bytes, vmatch_table_init);
 * nada se reserva en el bucle. Todo es lineal en los grupos y vértices.
 */
#ifndef SALSA64_RDP_VMATCH_H
#define SALSA64_RDP_VMATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

/* Por qué un grupo quedó sin pareja (vmatch_group.why). */
enum vmatch_why {
	VMATCH_OK,         /* con pareja */
	VMATCH_NEW,        /* no estaba en el cuadro anterior (ni su dirección ni su contenido) */
	VMATCH_MOVED,      /* se movió más que max_disp (corte, teletransporte) */
	VMATCH_JUMPED,     /* instancia que se movió mucho más que sus hermanas */
	VMATCH_DEFORMED,   /* grupo chico que se deformó de golpe */
	VMATCH_SCALE,      /* por el contenido, pero de otro tamaño en pantalla */
	VMATCH_FLAT,       /* proyección ortográfica (HUD 3D) */
	VMATCH_COLLAPSE,   /* a media interpolación se aplastaría: giró más de un cuarto de vuelta */
	VMATCH_OBJECT,     /* tenía pareja, pero se rechazó la mayor parte de su objeto (vmatch_atomic) */
	VMATCH_COMPONENT,  /* tenía pareja, pero otro grupo de su malla no (vmatch_atomic) */
	VMATCH_WHY_COUNT
};

struct vmatch_group {
	uint32_t addr, hash;   /* dirección en la lista del juego y hash del contenido (0: sin) */
	uint32_t occ, hocc;    /* repeticiones antes en el cuadro: por dirección y por contenido */
	uint32_t akey, hkey;   /* su lugar en vmatch_table.keys (hkey: ~0u sin hash) */
	uint32_t n, v0, stride;
	uint32_t clip;         /* primer vértice en vmatch_table.clip */
	float vp[4];           /* vscale x, y y vtrans x, y */
	float vz[2];           /* z de salida = vz[1]·w + vz[0]·z (gfx_vgroup.vz) */
	float cx, cy, ext;     /* centro y tamaño en pantalla (píxeles) de sus vértices visibles; ext < 0: ninguno */
	uint32_t flat;         /* proyección ortográfica (w = 1 en todos): el HUD 3D; no se interpola */
	uint32_t front;        /* algún vértice delante de la cámara (w > 0) */
	uint32_t why;          /* enum vmatch_why */
	int32_t pair;          /* grupo del cuadro anterior (vmatch_pair), o -1 */
	int32_t cand;          /* rechazado por deformarse o aplastarse: la pareja que tenía (vmatch_atomic puede devolvérsela), o -1 */
	uint32_t obj;          /* primer grupo de su objeto (vmatch_atomic, si hubo rechazados) */
};

/* Una clave (dirección o contenido) y los grupos que la tienen: order[start
 * … start + count) (los de contenido, desde vmatch_table.gcap). */
struct vmatch_key {
	uint32_t v, kind;      /* kind: generación de la tabla << 2 | 1 dirección o 2 contenido; otra generación: libre */
	uint32_t count, start;
	float mdx, mdy;        /* desplazamiento típico de sus instancias (mediana; vmatch_pair) */
	uint32_t has_m;        /* mdx, mdy valen */
	uint32_t pad;
};

struct vmatch_table {
	struct vmatch_group *g;
	float (*clip)[4];
	struct vmatch_key *keys;  /* potencia de 2 */
	uint32_t *order;          /* 2 × gcap */
	uint32_t *live;           /* 2 × gcap: las claves de esta generación (lugares en keys) */
	uint32_t nlive;
	uint8_t *taken;           /* gcap: grupos del anterior ya emparejados (vmatch_pair) */
	uint32_t *parent;         /* gcap: mallas (vmatch_atomic) */
	uint32_t gcap, ccap, kcap;
	uint32_t gen;             /* generación de las claves (vmatch_build): no hace falta borrar la tabla */
	uint32_t ngroups, nclip;
	bool full;                /* no cupieron todos los registros: el cuadro no se interpola */
};

struct vmatch_stats {
	uint32_t groups, vertices;       /* del cuadro */
	uint32_t found, found_vertices;  /* con la misma identidad en el anterior */
	uint32_t paired, paired_vertices;
	uint32_t moved;                  /* con pareja pero rechazados por el desplazamiento */
	uint32_t jumped, deformed;       /* rechazados: grupo chico que saltó, grupo que se deformó */
	uint32_t collapsed;              /* rechazados: se aplastarían a media interpolación (giros grandes) */
	uint32_t reordered;              /* emparejados con otra instancia que la de su orden (más cerca) */
	uint32_t flat;                   /* grupos con proyección ortográfica (HUD 3D): sin interpolar */
	uint32_t atomic;                 /* grupos con pareja que salen sin interpolar por su objeto o su malla (vmatch_atomic) */
	uint32_t kept;                   /* deformados o aplastados que se interpolan con su objeto (vmatch_atomic) */
	uint32_t by_content;             /* encontrados por el contenido (no por la dirección) */
	bool cut;                        /* el cuadro no se interpola */
};

/* Bytes de la memoria de una tabla de gcap grupos y ccap vértices. */
size_t vmatch_table_bytes(uint32_t gcap, uint32_t ccap);
/* La tabla sobre `mem` (vmatch_table_bytes, alineada a 16). */
void vmatch_table_init(struct vmatch_table *t, void *mem, uint32_t gcap, uint32_t ccap);
/* La tabla del cuadro desde los comandos de su tarea (cmd_bytes bytes). El
 * hash del contenido de cada grupo es el de su registro (lo calcula la HLE
 * con los vértices que acaba de leer): no hace falta mirar la RDRAM, que
 * cuando se traduce el juego ya puede haber cambiado. */
void vmatch_build(struct vmatch_table *t, const uint8_t *cmd, uint32_t cmd_bytes);
/* Empareja `cur` con `prev` (NULL: ninguno) y deja en cur->g[].pair el
 * resultado. max_disp en píxeles de la N64 por cuadro del juego. */
void vmatch_pair(const struct vmatch_table *prev, struct vmatch_table *cur, float max_disp, struct vmatch_stats *st);
/* Nunca un objeto a medias: en vez de mezclar partes interpoladas con
 * partes en el cuadro nuevo
 *   - un objeto: grupos seguidos en el orden de dibujo que se tocan en
 *     pantalla (los miembros de un personaje, cada uno con su malla y su
 *     dibujo). Si la mayor parte de sus vértices visibles quedó rechazada
 *     (Link que se da vuelta en un cuadro: casi todos sus miembros se
 *     aplastarían), sale entero como en el cuadro de ahora; si es una parte
 *     menor (el brazo y la espada en un tajo), las piezas que se deformaban
 *     o aplastaban se interpolan con las demás: interpoladas, dos piezas
 *     unidas por una articulación siguen unidas, y en el cuadro de ahora se
 *     despegarían
 *   - una malla: los grupos que comparten triángulos (los nvtx vértices de
 *     salida, de a tres), si uno delante de la cámara quedó sin pareja
 *     (también si es nuevo: sus triángulos con los demás se estirarían)
 * Entre vmatch_pair y vmatch_prev. */
void vmatch_atomic(struct vmatch_table *cur, const struct gfx_vertex *vtx, uint32_t nvtx, struct vmatch_stats *st);
/* Las posiciones en el cuadro anterior de los nvtx vértices de salida de la
 * tarea (mismo formato que gfx_vertex: x, y, z, w de salida), en `out`
 * (nvtx × 4 floats); devuelve cuántos tienen pareja. */
uint32_t vmatch_prev(const struct vmatch_table *prev, const struct vmatch_table *cur, const struct vmatch_stats *st,
		     const struct gfx_vertex *vtx, uint32_t nvtx, float (*out)[4]);

#endif
