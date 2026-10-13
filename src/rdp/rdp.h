/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — RDP sobre el RSX: consume la salida de la HLE gráfica
 * (src/rsp/gfx/gfx.h) y la dibuja con lib/rsxkit.
 *
 * Cada color image de la N64 (dirección en la RDRAM) tiene su destino de
 * render en el RSX, a una escala entera de la resolución de la N64 propia
 * de ese destino (3× a 720p para un framebuffer de 320 × 240; ver
 * salsa64_rdp_init); cada z image, su profundidad. Al presentar un campo, si el VI
 * muestra una color image que dibujó el RSX, se presenta ese destino; si
 * no (imagen hecha por el CPU), el frontend usa el framebuffer 2D. Qué
 * destino es ese, y cuál se recicla cuando no caben más, lo decide
 * targets.c (portable, con pruebas en tests/unit/test_targets.c).
 *
 * Los búferes de salida de la HLE (vértices, comandos, texturas) viven en
 * memoria principal mapeada al RSX, en varios juegos que se turnan: el SPE
 * escribe uno mientras el RSX dibuja otro. Una barrera del RSX dice cuándo
 * se puede volver a usar cada juego. La caché de texturas (gfx.h) es otra
 * región mapeada que la HLE administra sola; las barreras le dicen qué
 * tareas ya terminó el RSX (job->done_task).
 *
 * Solo PS3 (PSL1GHT). Dos hilos (docs/hilos-ppe.md): el sink (acquire,
 * done) se llama desde el hilo de emulación; lo demás, desde el hilo que
 * usa rsxkit (presentación). Las tareas terminadas esperan en una cola
 * hasta salsa64_rdp_flush, que las traduce en orden.
 */
#ifndef SALSA64_RDP_RDP_H
#define SALSA64_RDP_RDP_H

#include <rsxkit/rsxkit.h>

#include "core/rcp.h"
#include "rsp/gfx/backend.h"

struct salsa64_rdp_stats {
	uint32_t tasks;
	uint32_t draws, triangles;
	uint32_t targets_created;
	uint32_t targets_evicted;  /* destinos reciclados por el tope de memoria */
	uint32_t set_waits;      /* veces que un juego de búferes seguía en uso (emulación) */
	uint64_t us_translate;   /* PPE traduciendo a comandos del RSX (presentación) */
	uint64_t us_set_wait;    /* hilo de emulación esperando un juego */
	/* Antigüedad de lo presentado desde el RSX: campos entre el dibujo del
	 * destino y el campo que lo muestra (la latencia propia del juego: 2 con
	 * doble búfer a 30 cps), sumada y la mayor. */
	uint64_t age_sum;
	uint32_t age_max;
	/* Fragment programs generados (lib/rsxkit, rsxkit_fp_stats): dibujos
	 * que usaron uno, programas distintos en la caché, generados en total,
	 * vaciados de la caché y cargas de programa (cambios, contando los
	 * precompilados). */
	uint32_t draws_generated;
	uint32_t fp_programs, fp_generated, fp_evictions, fp_loads;
	/* Efectos de framebuffer (docs/framebuffer.md): copias RSX → RDRAM y
	 * su tiempo en el hilo de presentación (esperar al RSX y convertir),
	 * tareas que la emulación esperó por una copia y cuánto, texturas con
	 * origen en un destino leídas de su superficie o de la RDRAM, y
	 * destinos cebados desde la RDRAM. */
	uint32_t readbacks, readback_waits, readback_late;
	uint64_t us_readback, us_readback_wait;
	uint32_t mirrors, mirrors_skipped;  /* copias espejo (sin esperar) y las que no cupieron */
	uint32_t frames_mirrored;           /* campos cuyo framebuffer se copió a la RDRAM */
	uint32_t dma_skipped;               /* copias descartadas porque un DMA escribió ahí después */
	uint32_t tall_fills;                /* rellenos de una color image más alta que 480 repartidos a sus destinos */
	uint32_t tex_rsx, tex_rdram, tex_self, primes;
	/* Texturas que la HLE no decodificó por venir de un destino
	 * (GFX_TEX_TARGET) y que no se pudieron leer de él: decodificadas en el
	 * PPE desde la RDRAM, y las que no cupieron en el arena (dibujo saltado). */
	uint32_t tex_ppe, tex_ppe_lost;
	/* Copias pedidas que no encontraron su destino dibujado, superficies
	 * que el RSX no pudo crear y color images imposibles ignoradas. */
	uint32_t readbacks_lost, targets_failed, targets_absurd;
	/* Dibujos saltados porque rsxkit no ligó su destino (ninguna
	 * profundidad cubría la superficie; no debería pasar, ver
	 * salsa64_rdp_init). */
	uint32_t binds_failed;
};

/* Después de rsxkit_init. scale = 0 (automática): la mayor escala entera
 * que cabe en la pantalla (720p → 3×, 1080p → 4×); 1 .. 5: la pedida
 * (1× es la imagen nativa de 320 × 240; más que la pantalla es
 * supermuestreo: la presentación la reduce con filtro lineal). Devuelve 0
 * o un código negativo de rsxkit.
 *
 * La escala elegida es un objetivo de líneas (240 × escala: 5× = 1200) y
 * cada destino usa la escala entera que lleva su imagen a ese objetivo sin
 * pasarse (rdp_target_scale): 320 × 240 a 5×, 640 × 480 a 2×, una auxiliar
 * chica a la misma escala que su alto pida (hasta 5×). Cada superficie lleva
 * su escala (surf_scale), y la profundidad la del destino de color con que
 * se usa; rsxkit ya trabaja en coordenadas lógicas (rsxkit_bind). Todo lo
 * que pasa de coordenadas lógicas a píxeles de una superficie usa la escala
 * de esa superficie (rdp_surface_rect, rdp_target_tex_map): la
 * presentación, las copias RSX → RDRAM (reducen a 1× desde la escala del
 * destino), el espejo del framebuffer, el render a textura (un destino
 * leído como textura desde otro de otra escala: u y v van normalizadas) y
 * el scratch (copia del ligado, con su tamaño y su escala).
 *
 * Destinos sin z image propia. El RSX siempre liga una zeta junto al color;
 * un destino que se dibuja sin z image (rectángulos, 2D, el cebado, el
 * scratch) se liga con la profundidad de la pantalla, con la prueba y la
 * escritura apagadas (lo que el juego espera sin z image). A 4×–5× en 720p
 * (o 5× en 1080p) un destino es más grande que la pantalla, y una zeta más
 * chica que el color puede fallar o recortar en el RSX. Se eligió agrandar
 * la profundidad de la pantalla (rsxkit_depth_reserve) al mayor destino
 * posible a la escala, al iniciar y en salsa64_rdp_set_scale, fuera del
 * bucle: es lo más simple (un búfer que ya existía, sin tocar los
 * dibujos) y lo más seguro (el RSX queda siempre con una zeta que cubre al
 * color, la configuración que ya corre en la PS3 a 3× en 720p y a 4× en
 * 1080p). «Sin zeta» no es una opción limpia: la superficie del RSX
 * (gcmSurface) siempre lleva formato, dirección y pitch de zeta, y una
 * zeta de pitch 64 (lo que se usa para los colores sin usar) seguiría
 * siendo más chica que el color, que es justo el riesgo; una profundidad
 * por destino costaría una superficie más por ranura.
 * Cuesta a lo más una superficie del mayor tamaño (7,7 MB a 5×; en 720p,
 * 4 MB más que la de la pantalla), contada en el tope. Además rsxkit_bind
 * se niega a ligar con una zeta más chica (RSXKIT_EDEPTH) y el dibujo se
 * salta (binds_failed): nunca pasa en silencio. Política en targets.c
 * (rdp_zeta_reserve, con pruebas).
 *
 * Política de memoria local del RSX (docs/memory-budget.md):
 * 1) al arrancar el objetivo se baja hasta que el peor caso (8 destinos, el
 * scratch, 4 profundidades y la profundidad de la pantalla agrandada, de
 * 320 × 240 a esa escala; rdp_vram_worst) quepa en RDP_VRAM_BUDGET
 * (112 MiB): a 5× son 102,5 MiB y entra; 2) en el juego, lo vivo suma a lo
 * más RDP_VRAM_BUDGET: para crear una superficie que pasaría se suelta el
 * scratch y se recicla el destino más viejo (no el presentado en el último
 * campo ni uno que una textura pueda estar leyendo); si ni así cabe, o
 * rsxkit no tiene memoria, esa imagen se queda en la copia de la RDRAM y no
 * se dibuja en el RSX, sin errores. */
int salsa64_rdp_init(uint32_t scale);
/* Escala-objetivo efectiva tras salsa64_rdp_init; *reduced: se bajó por memoria. */
uint32_t salsa64_rdp_scale(bool *reduced);
/* Cambia la escala pedida (0: automática) con el RDP ya iniciado y sin juego
 * abierto: suelta los destinos, el scratch y las profundidades, descarta las
 * copias a la RDRAM en vuelo y recalcula la escala-objetivo y la
 * profundidad de la pantalla con la pantalla en curso (después de
 * rsxkit_video_reconfigure). Conserva la memoria mapeada (juegos de
 * búferes, caché de texturas y búferes de las copias). */
void salsa64_rdp_set_scale(uint32_t scale);
void salsa64_rdp_shutdown(void);
/* Entre juegos (menú): suelta los destinos y profundidades del RSX y vacía
 * las colas de tareas y las estadísticas; la memoria mapeada se conserva.
 * Sin backend gráfico conectado (salsa64_rsp_set_gfx(NULL)) y después del
 * último salsa64_rdp_flush. Devuelve las tareas que quedaban sin traducir
 * (0 si se consumió todo). */
uint32_t salsa64_rdp_reset(void);
/* Estados guardados, en la barrera (después del último salsa64_rdp_flush,
 * con la emulación quieta): settle espera las copias RSX → RDRAM que
 * quedaran en vuelo (espejos del framebuffer y de las imágenes chicas,
 * copias grandes sin esperar) y las escribe en la RDRAM, así ninguna llega
 * tarde. drop_targets, al cargar: suelta los destinos, profundidades y el
 * scratch (lo dibujado ya no es de esta máquina y los números de campo
 * retroceden) y olvida las copias y su historial, sin tocar las colas ni la
 * numeración de las tareas, que siguen con el mismo SPE. */
void salsa64_rdp_settle(void);
void salsa64_rdp_drop_targets(void);

/* Efectos de framebuffer (docs/framebuffer.md). La RDRAM emulada, para
 * copiar ahí los destinos que el CPU va a leer y cebar superficies con lo
 * que el CPU dibujó, y cómo despertar al hilo de presentación cuando una
 * tarea necesita esa copia antes de terminar (`done` espera a que esté).
 * Antes de la primera tarea; NULL apaga las copias. */
void salsa64_rdp_set_memory(uint8_t *rdram, uint32_t rdram_size, void (*wake)(void));
/* Hilo de emulación, al terminar cada campo con imagen: origen y ancho del
 * VI, para distinguir framebuffers de imágenes auxiliares desde ese hilo. */
void salsa64_rdp_vi(uint32_t origin, uint32_t width);
/* Interruptor: 0 apaga los efectos de framebuffer (las texturas que
 * vienen de un destino se leen de la RDRAM como antes y nada se copia), 1
 * solo render a textura, 2 también las copias a la RDRAM (por omisión). */
void salsa64_rdp_fbfx(int level);
/* Hilo de emulación: un DMA escribió la RDRAM [pa, pa + len); con
 * `gfx_in_flight`, mientras corría una tarea gráfica. Las copias de esa
 * tarea y de las anteriores no pisan ese tramo (targets.h, rdp_dma_ring). */
void salsa64_rdp_rdram_written(uint32_t pa, uint32_t len, bool gfx_in_flight);
/* Lo que eligió la última salsa64_rdp_present: ranura (-1: nada del RSX),
 * campos desde que se dibujó y dirección del destino (traza por campo). */
void salsa64_rdp_last_present(int *slot, uint32_t *age, uint32_t *addr);

/* Avisos y la traza de las copias (los escribe el frontend en su log). */
void salsa64_rdp_set_log(void (*fn)(const char *fmt, ...));

/* Las dos manos de un backend gráfico (rsp/gfx/backend.h). acquire puede
 * esperar a que el RSX libere un juego de búferes (nunca llama a rsxkit
 * salvo rsxkit_fence_done). done puede esperar a que el hilo de
 * presentación copie un destino a la RDRAM (ver salsa64_rdp_set_memory). */
const struct salsa64_gfx_sink *salsa64_rdp_sink(void);

/* Traduce a comandos del RSX las tareas terminadas con número menor que
 * `upto` (~0u: todas) y, siempre, la que la emulación espera en done por
 * una copia a la RDRAM. Devuelve cuántas van traducidas en total. */
uint32_t salsa64_rdp_flush(uint32_t upto);
/* Tareas terminadas (done) hasta ahora; para el hilo de emulación, que lo
 * manda con cada campo. */
uint32_t salsa64_rdp_tasks_done(void);

/* Recorte del overscan al presentar, en píxeles de la imagen del VI por
 * cada lado (0: sin recorte). */
struct salsa64_overscan {
	uint32_t left, right, top, bottom;
};

/* Si el VI muestra una color image dibujada por el RSX en los últimos
 * campos, la presenta (en el cuadro en curso de rsxkit), sin los bordes de
 * `crop` (NULL: entera), y devuelve true. */
bool salsa64_rdp_present(const struct salsa64_vi_frame *f, uint32_t field, const rsxkit_view *view,
			 const struct salsa64_overscan *crop, rsxkit_rect *out);
/* Con `draw` en false, salsa64_rdp_present elige qué se ve (y hace todo lo
 * demás) pero no lo dibuja: lo dibuja quien llama, con un filtro (desde
 * salsa64_rdp_present_source) o sin él (salsa64_rdp_present_again). */
void salsa64_rdp_present_draw(bool draw);
/* Lo último que eligió salsa64_rdp_present como textura (el destino
 * entero), la región que se ve (en píxeles del destino, sin el overscan) y
 * la escala de ese destino; con `view` o `crop`, antes los aplica como
 * salsa64_rdp_present_again (en la pausa). false si no hay. */
bool salsa64_rdp_present_source(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_texture *tex,
				rsxkit_rect *region, uint32_t *scale);
/* Vuelve a dibujar lo último que presentó salsa64_rdp_present (mismo
 * destino, región y escala), sin tocar la tabla de destinos ni las copias a
 * la RDRAM: para seguir mostrando el último cuadro con la emulación en
 * pausa. Con `view` o `crop` (si no, NULL), con esa vista y ese recorte (el
 * menú de pausa los cambia en vivo; las dos cosas se piden juntas: un NULL
 * es «sin recorte» o «la vista por omisión»). false si no hay (nada
 * presentado, o el destino se soltó). */
bool salsa64_rdp_present_again(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_rect *out);

void salsa64_rdp_get_stats(struct salsa64_rdp_stats *out);

/* Tiempo del hilo que traduce (presentación), en ticks del timebase: la
 * traducción entera (con lo que sigue cuando cae dentro de ella), las copias
 * que convierte el PPE hacia la RDRAM (copias, espejos, cebado, VISCVG), las
 * texturas decodificadas en el PPE y las esperas a una barrera del RSX
 * (dormido). Lo mide docs/hilos-ppe.md, «Cuánto le quita el hilo 1». */
struct salsa64_rdp_tb {
	uint64_t translate, copy, tex, fence;
};
void salsa64_rdp_get_tb(struct salsa64_rdp_tb *out);

/* Escribe la salida de la última tarea (gfx.h: gfx_dump_header) en `path`,
 * con las texturas de la caché copiadas al arena (el volcado no depende de
 * la caché). Espera al RSX. Para depurar. */
bool salsa64_rdp_dump(const char *path);

#endif
