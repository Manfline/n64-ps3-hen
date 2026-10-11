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
/* Profundidades vivas (rdp.c; Zelda en pausa alterna tres z-buffers: el
 * principal y uno por par de búferes de Link). */
#define RDP_DEPTHS_MAX    4
/* Superficies del RSX que el RDP puede tener vivas a la vez: una por
 * ranura, el scratch de los efectos de framebuffer (copia del destino
 * ligado cuando un dibujo lo lee), las profundidades y la profundidad de
 * la pantalla agrandada para ligar sin z image destinos más grandes que la
 * pantalla (rdp_zeta_reserve). lib/rsxkit admite 16 destinos y 16
 * profundidades: sobra. */
#define RDP_SURFACES_MAX  (RDP_TARGETS_MAX + 1 + RDP_DEPTHS_MAX + 1)
/* Escala interna máxima (320 × 240 × 5 = 1600 × 1200; 640 × 5 = 3200 cabe
 * en el límite de 4096 de las superficies del RSX). */
#define RDP_SCALE_MAX     5
/* Tope de memoria local del RSX para destinos, scratch, profundidades y la
 * profundidad de la pantalla agrandada (ver docs/memory-budget.md): el peor
 * caso a 5× (RDP_SURFACES_MAX de 1600 × 1200, 102,5 MiB) entra con 9,5 MiB
 * de sobra (con 96 MiB, 5× se bajaba a 4×); rdp.c recicla el destino más
 * viejo si aun así no cupiera. */
#define RDP_VRAM_BUDGET   (112u << 20)
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
	/* Lo dibujado ya está en la RDRAM por el espejo del framebuffer
	 * (rdp_targets_mirrored), o no hay nada dibujado que copiar (recién
	 * creado). Lo borra el próximo dibujo. */
	bool mirrored;
	uint8_t fmt;          /* G_IM_FMT_* de la color image (lo pone quien la crea; 0 si no se sabe) */
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

/* Bytes de una superficie de w × h píxeles de 32 bits (pitch alineado a
 * 128, como lib/rsxkit). */
uint32_t rdp_surface_bytes(uint32_t w, uint32_t h);

/* Peor caso a la escala-objetivo `scale`: cada superficie mide a lo más
 * 320·scale × 240·scale (el alto lógico de un destino es al menos 3/4 del
 * ancho y rdp_target_scale no pasa de 240·scale líneas; a 1× una de
 * 640 × 480 queda nativa, 1,2 MB), también el scratch (copia de un
 * destino), cada profundidad (la del destino con que se usa, o la mayor de
 * dos si la comparten) y la profundidad de la pantalla agrandada
 * (rdp_zeta_reserve; se cuenta entera aunque la pantalla ya tuviera una):
 * RDP_SURFACES_MAX superficies (8 + 1 + 4 + 1). A 5× son 107,5 MB
 * (102,5 MiB); a 3×, 38,7 MB; a 4×, 68,8 MB. */
uint32_t rdp_vram_worst(uint32_t scale);

/* Lo más que mide una superficie a la escala-objetivo `scale` (destino,
 * scratch o profundidad): max(320·scale, 640) × max(240·scale, 480). */
void rdp_surface_max(uint32_t scale, uint32_t *w, uint32_t *h);

/* Profundidad de la pantalla para los destinos que se ligan sin z image
 * propia (dibujos sin z, cebado, scratch): rsxkit_bind no liga un destino
 * más grande que ella (una zeta más chica que el color puede fallar o
 * recortar en el RSX). Si a esta escala algún destino puede ser más grande
 * que la pantalla de screen_w × screen_h (4× y 5× en 720p, 5× en 1080p),
 * devuelve true con el tamaño a reservar (rsxkit_depth_reserve: el mayor
 * destino posible, rdp_surface_max); si no, false y 0 × 0 (la de la
 * pantalla alcanza). */
bool rdp_zeta_reserve(uint32_t scale, uint32_t screen_w, uint32_t screen_h, uint32_t *w, uint32_t *h);

/* Región lógica (x, y, w, h) de un destino a escala `scale`, en píxeles
 * de su superficie: lo que leen las copias RSX → RDRAM (que la reducen a
 * w × h), el espejo del framebuffer y la presentación. Siempre con la
 * escala del destino que se lee, no la objetivo. */
struct rdp_rect {
	uint32_t x, y, w, h;
};
void rdp_surface_rect(uint32_t scale, uint32_t x, uint32_t y, uint32_t w, uint32_t h, struct rdp_rect *out);

/* Render a textura: una textura de la HLE de tex_w × tex_h texels que
 * empieza en el texel (x0, y0) de un destino de `lw` puntos lógicos de
 * ancho, cuya superficie está a escala `scale`, con u = s·xf0 + xf2
 * normalizada a tex_w. Se liga como textura desde la fila row0 de la
 * superficie, `rows` filas (filas enteras: el offset de una textura va
 * alineado a 128), y u pasa a normalizarse a la fila entera del destino
 * (xf0, xf2 de salida). u y v quedan normalizadas, así que el destino en
 * que se dibuja puede tener otra escala sin cambiar nada más. */
struct rdp_tex_map {
	uint32_t row0, rows;
	float xf0, xf2;
};
void rdp_target_tex_map(uint32_t scale, uint32_t lw, uint32_t x0, uint32_t y0, uint32_t tex_w, uint32_t tex_h,
			float xf0, float xf2, struct rdp_tex_map *out);

/* ¿La región de src_w × src_h píxeles de superficie se presenta a un
 * múltiplo entero (sin el estiramiento fraccionario que repetía filas con
 * el recorte del overscan) en vez de ajustarse a la pantalla? Sí si cabe y
 * el múltiplo llena al menos 3/4 del alto de la pantalla (960 × 720 o
 * 912 × 684 en 720p, 1280 × 960 en 1080p, 320 × 240 a 1× por 3); no si
 * quedaría chica (640 × 480 en 720p, 960 × 720 en 1080p) o no cabe
 * (supermuestreo: se reduce con filtro lineal). */
bool rdp_present_integer(uint32_t src_w, uint32_t src_h, uint32_t screen_w, uint32_t screen_h);

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

/* Espejo del framebuffer (rdp.c y el rasterizador del host): lo que el RDP
 * dibuja en un framebuffer se copia a la RDRAM para que el CPU lea el
 * cuadro y no basura (la cámara del túnel de Mario Kart), una sola vez por
 * dibujo y cuanto antes: al terminar la tarea que lo dibujó si ya se sabe
 * que es un framebuffer (rdp_targets_mirror_at_task_end: el VI lo mostró
 * alguna vez, de 16 bits y del ancho del VI) y, si no, al presentarlo
 * (rdp_targets_mirror_wanted). Copiar al presentar, o en cada campo, un
 * cuadro que el RDP dibujó antes pisa lo que el CPU o un DMA escribieron
 * entre medio: Super Mario 64, al pasar del logo a la cara de Mario,
 * dibuja un framebuffer, carga por DMA datos encima de él y de otro y el
 * VI los muestra uno o dos campos más; esas copias pisaban los datos y el
 * juego se colgaba (dejaba de mandar tareas gráficas). */
bool rdp_targets_mirror_wanted(const struct rdp_targets *s, int slot);
bool rdp_targets_mirror_at_task_end(const struct rdp_targets *s, int slot);
/* La copia del espejo de la ranura se hizo (o quedó encolada). */
void rdp_targets_mirrored(struct rdp_targets *s, int slot);

void rdp_vi_ring_push(struct rdp_vi_ring *r, uint32_t origin, uint32_t width);
/* ¿Hay que copiar a la RDRAM la color image (addr, width, siz) cuando el
 * RDP termine de dibujarla? Sí para una imagen de 16 bits del ancho del VI
 * que no es ninguno de los framebuffers recientes: un cuadro guardado
 * aparte, que el CPU suele leer (el fondo de la pausa de Zelda). Las
 * imágenes chicas (iconos, pantallas) se quedan en el RSX como textura. */
bool rdp_readback_wanted(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t siz);
/* ¿La color image (addr, width) está dentro de alguno de los framebuffers
 * recientes del VI? Las que no lo están son imágenes auxiliares. */
bool rdp_vi_ring_covers(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width);

/* Qué presentar en el campo `field` para la imagen `f` del VI: entre los
 * destinos recientes que cubren el origen, el del ancho del VI; entre
 * iguales, el que empieza más cerca del origen y, si empatan, el dibujado
 * hace menos. Si no hay ninguno pero el VI sigue en el mismo origen que
 * el campo anterior, se sostiene el destino presentado entonces (el juego
 * deja de dibujar mientras carga). */
void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out);

#endif
