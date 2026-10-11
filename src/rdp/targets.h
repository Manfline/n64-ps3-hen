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
/* Un destino sin dibujar en tantos campos ya no se presenta (ni se protege
 * del reciclado). */
#define RDP_TARGETS_STALE 30

/* Un destino dibujado hace más de tantas tareas ya no se lee del RSX como
 * textura: lo que vale es la RDRAM (docs/framebuffer.md). Dos o tres por
 * los framebuffers alternos (Zelda copia el suyo dos tareas después). */
#define RDP_TARGETS_FRESH 3

struct rdp_target {
	bool used;
	bool drawn;           /* dibujado desde la última presentación */
	bool presented;       /* el VI lo mostró alguna vez: es un framebuffer */
	/* Se copió a la RDRAM al terminar una tarea (rdp_targets_readback):
	 * desde entonces la verdad está ahí (el CPU puede haberla cambiado)
	 * hasta que el RDP lo vuelva a dibujar. */
	bool cpu_owned;
	uint32_t addr;        /* color image: dirección física y ancho en píxeles */
	uint32_t width;
	uint32_t bpp;         /* bytes por píxel: 1, 2 o 4 */
	uint32_t lw, lh;      /* tamaño lógico (píxeles de la N64) de la superficie */
	uint32_t last_field;  /* último campo en que se dibujó (o se creó) */
	uint32_t last_task;   /* última tarea que lo dibujó */
};

struct rdp_targets {
	struct rdp_target t[RDP_TARGETS_MAX];
	uint32_t field;         /* campo en curso: el último presentado */
	uint32_t task;          /* tarea gráfica en curso (rdp_targets_task) */
	int shown;              /* ranura presentada en el último campo, o -1 */
	uint32_t shown_origin;  /* origen del VI en ese campo */
	uint32_t shown_field;
	uint32_t vi_width;      /* ancho del VI en el último campo con imagen */
	uint32_t created;       /* superficies creadas (estadística) */
	uint32_t readbacks;     /* copias a la RDRAM (estadística) */
	uint32_t tex_rsx, tex_rdram, tex_self;  /* texturas con origen en un destino: del RSX, de la RDRAM, del ligado */
};

/* Orígenes recientes del VI (campos con imagen), para saber desde el hilo
 * que no tiene la tabla si una color image es un framebuffer o una imagen
 * auxiliar: lo lleva quien corre el VI (rdp_vi_ring_push por campo). */
#define RDP_VI_RING 8
struct rdp_vi_ring {
	uint32_t origin[RDP_VI_RING], width[RDP_VI_RING];
	uint32_t n;
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

/* Ranura del destino que contiene la dirección `addr` de una imagen de
 * `pitch` texels por fila y `bpp` bytes por texel (el origen de una textura:
 * gfx_tex.src_*), con el texel (x0, y0) del destino en que cae; -1 si
 * ninguno. Entre destinos que se solapan, el que empieza más cerca. No
 * mira la antigüedad: quien llama decide con last_task. */
int rdp_targets_locate(const struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t *x0,
		       uint32_t *y0);

/* Empieza una tarea gráfica (antes de traducir sus comandos). */
void rdp_targets_task(struct rdp_targets *s);

/* De dónde leer una textura cuyo origen es (addr, pitch, bpp): de la
 * superficie del RSX del destino `*slot` (render a textura: dibujado hace
 * RDP_TARGETS_FRESH tareas o menos y no es del CPU), de una copia del
 * destino ligado `cur` (SELF: el juego lee lo que está dibujando, como la
 * pausa de Zelda sobre su framebuffer; quien llama copia la superficie a un
 * scratch antes), de la RDRAM (la textura decodificada por la HLE: el
 * destino es viejo o se copió a la RDRAM) o de ninguno (no cae en un
 * destino). Cuenta en tex_rsx / tex_rdram / tex_self. */
enum rdp_texsrc { RDP_TEXSRC_NONE = 0, RDP_TEXSRC_RSX, RDP_TEXSRC_RDRAM, RDP_TEXSRC_SELF };
enum rdp_texsrc rdp_targets_texture(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, int cur,
				    int *slot, uint32_t *x0, uint32_t *y0);

/* Al terminar una tarea que dibujó la ranura: si hay que copiarla a la
 * RDRAM (rdp_readback_wanted con los orígenes del VI de la tabla) la marca
 * del CPU y devuelve true. Con `force`, se copia sin preguntar (lo decidió
 * otro hilo con rdp_readback_wanted). */
bool rdp_targets_readback(struct rdp_targets *s, int slot, bool force);

void rdp_vi_ring_push(struct rdp_vi_ring *r, uint32_t origin, uint32_t width);
/* ¿Hay que copiar a la RDRAM la color image (addr, width, siz) cuando el
 * RDP termine de dibujarla? Sí para una imagen de 16 bits del ancho del VI
 * que no es ninguno de los framebuffers recientes: un cuadro guardado
 * aparte, que el CPU suele leer (el fondo de la pausa de Zelda). Las
 * imágenes chicas (iconos, pantallas) se quedan en el RSX como textura. */
bool rdp_readback_wanted(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t siz);

/* Qué presentar en el campo `field` para la imagen `f` del VI: entre los
 * destinos recientes que cubren el origen, el del ancho del VI; entre
 * iguales, el que empieza más cerca del origen y, si empatan, el dibujado
 * hace menos. Si no hay ninguno pero el VI sigue en el mismo origen que
 * el campo anterior, se sostiene el destino presentado entonces (el juego
 * deja de dibujar mientras carga). */
void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out);

#endif
