/*
 * Salsa64 — destinos de render del RDP: qué color image de la N64 tiene
 * superficie en el RSX, cuál se recicla cuando no caben más y cuál se
 * presenta cuando el VI muestra una dirección de la RDRAM.
 *
 * Portable (sin rsxkit) para probar la elección en el host
 * (tests/unit/test_targets.c); rdp.c guarda aparte las superficies de
 * rsxkit, una por ranura, y crea o destruye la de una ranura cuando
 * rdp_targets_get dice que cambió de imagen.
 *
 * Cada destino es una color image (dirección, ancho); su alto lógico es
 * una conjetura, porque la N64 no declara el alto: 3/4 del ancho, o más si
 * las tijeras de los dibujos que recibe llegan más abajo (la imagen
 * auxiliar de Conker es CI8 de 64 × 64: con 48 líneas, el RSX escribía
 * fuera de la superficie). Por eso dos framebuffers seguidos se solapan en
 * la cuenta: el que empieza más cerca del origen del VI es el que se
 * muestra (Banjo-Kazooie alterna dos de 292 × 216 y presentaba el otro, con
 * solo sus últimas líneas, cuando el RDP lo acababa de dibujar).
 */
#ifndef SALSA64_RDP_TARGETS_H
#define SALSA64_RDP_TARGETS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/rcp.h"

#define RDP_TARGETS_MAX   8
/* Escala interna máxima (320 × 240 × 5 = 1600 × 1200; 640 × 5 = 3200 cabe
 * en el límite de 4096 de las superficies del RSX). */
#define RDP_SCALE_MAX     5
/* Tope de memoria local del RSX para destinos y profundidades vivos (ver
 * docs/memory-budget.md); rdp.c recicla el destino más viejo para respetarlo. */
#define RDP_VRAM_BUDGET   (96u << 20)
/* Un destino sin dibujar en tantos campos ya no se presenta (ni se protege
 * del reciclado). */
#define RDP_TARGETS_STALE 30

struct rdp_target {
	bool used;
	bool drawn;           /* dibujado desde la última presentación */
	uint32_t addr;        /* color image: dirección física y ancho en píxeles */
	uint32_t width;
	uint32_t bpp;         /* bytes por píxel: 1, 2 o 4 */
	uint32_t lw, lh;      /* tamaño lógico (píxeles de la N64) de la superficie */
	uint32_t last_field;  /* último campo en que se dibujó (o se creó) */
};

struct rdp_targets {
	struct rdp_target t[RDP_TARGETS_MAX];
	uint32_t field;         /* campo en curso: el último presentado */
	int shown;              /* ranura presentada en el último campo, o -1 */
	uint32_t shown_origin;  /* origen del VI en ese campo */
	uint32_t shown_field;
	uint32_t vi_width;      /* ancho del VI en el último campo con imagen */
	uint32_t created;       /* superficies creadas (estadística) */
};

/* Lo que se presenta en un campo: la región lógica del destino que
 * muestra el VI. */
struct rdp_present {
	int slot;         /* -1: nada del RSX (va la copia de la RDRAM) */
	uint32_t y0;      /* primera línea lógica (el origen del VI dentro del destino) */
	uint32_t w, h;    /* píxeles lógicos */
	uint32_t age;     /* campos desde que se dibujó lo presentado (0: en este campo) */
};

void rdp_targets_init(struct rdp_targets *s);

/* Bytes de una superficie de w × h píxeles de 32 bits (pitch alineado a
 * 128, como lib/rsxkit). */
uint32_t rdp_surface_bytes(uint32_t w, uint32_t h);

/* Peor caso a la escala-objetivo `scale`: cada destino pesa a lo más una
 * superficie de 320 × 240 × scale² (rdp_target_scale lleva las de otros
 * tamaños a ≤ ese peso; a 1× una de 640 × 480 queda nativa, 1,2 MB), y hay hasta 8 color images y 2 profundidades:
 * 10 superficies. A 5× son 77 MB (73 MiB). */
uint32_t rdp_vram_worst(uint32_t scale);

/* Escala entera de un destino de lw × lh puntos lógicos para la escala-
 * objetivo `global` (1..RDP_SCALE_MAX): la mayor que no pasa de 240 × global
 * líneas, al menos 1, con tope RDP_SCALE_MAX y de 4096 por lado. */
uint32_t rdp_target_scale(uint32_t global, uint32_t lw, uint32_t lh);

/* Escala-objetivo efectiva. requested = 0: la mayor entera que cabe en una pantalla
 * de `screen_h` líneas (720 → 3, 1080 → 4); 1 .. RDP_SCALE_MAX: la pedida.
 * En ambos casos se baja hasta que rdp_vram_worst(escala) ≤
 * RDP_VRAM_BUDGET. *reduced: se bajó por memoria (no por el tope de 5 ni
 * por ser automática). */
uint32_t rdp_scale_pick(uint32_t requested, uint32_t screen_h, bool *reduced);
/* Igual con otro presupuesto (pruebas). */
uint32_t rdp_scale_pick_budget(uint32_t requested, uint32_t screen_h, uint32_t budget, bool *reduced);

/* Ranura de la color image (addr, width, siz de G_SETCIMG), existente o
 * reciclada, con al menos `min_lh` líneas (0: las de siempre, 3/4 del
 * ancho; quien llama pasa hasta dónde llegan las tijeras de los dibujos
 * que vienen). *fresh: la ranura cambió de imagen, es nueva o creció: hay
 * que crear su superficie de lw × lh (y destruir la anterior si la había).
 * Al reciclar, nunca se toma la ranura presentada en el último campo;
 * entre las demás se prefiere una vieja (RDP_TARGETS_STALE), después una
 * de un ancho distinto al del VI (imagen auxiliar, no un framebuffer) y
 * al final la dibujada hace más campos. */
int rdp_targets_get(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh,
		    bool *fresh);

/* Se dibujó en la ranura durante el campo en curso. */
void rdp_targets_drawn(struct rdp_targets *s, int slot);

/* Qué presentar en el campo `field` para la imagen `f` del VI: entre los
 * destinos recientes que cubren el origen, el del ancho del VI; entre
 * iguales, el que empieza más cerca del origen y, si empatan, el dibujado
 * hace menos. Si no hay ninguno pero el VI sigue en el mismo origen que
 * el campo anterior, se sostiene el destino presentado entonces (el juego
 * deja de dibujar mientras carga). */
void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out);

#endif
