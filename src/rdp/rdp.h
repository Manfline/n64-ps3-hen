/*
 * Salsa64 — RDP sobre el RSX: consume la salida de la HLE gráfica
 * (src/rsp/gfx/gfx.h) y la dibuja con lib/rsxkit.
 *
 * Cada color image de la N64 (dirección en la RDRAM) tiene su destino de
 * render en el RSX, a una escala entera de la resolución de la N64 (3× a
 * 720p); cada z image, su profundidad. Al presentar un campo, si el VI
 * muestra una color image que dibujó el RSX, se presenta ese destino; si
 * no (imagen hecha por el CPU), el frontend usa el framebuffer 2D.
 *
 * Los búferes de salida de la HLE (vértices, comandos, texturas) viven en
 * memoria principal mapeada al RSX, en varios juegos que se turnan: el SPE
 * escribe uno mientras el RSX dibuja otro. Una barrera del RSX dice cuándo
 * se puede volver a usar cada juego. La caché de texturas (gfx.h) es otra
 * región mapeada que la HLE administra sola; las barreras le dicen qué
 * tareas ya terminó el RSX (job->done_task).
 *
 * Solo PS3 (PSL1GHT). Todas las llamadas desde el hilo que usa rsxkit.
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
	uint32_t set_waits;      /* veces que un juego de búferes seguía en uso */
	uint64_t us_translate;   /* PPE traduciendo a comandos del RSX */
	uint64_t us_set_wait;
};

/* Después de rsxkit_init. scale = 0: la mayor escala entera que cabe en la
 * pantalla. Devuelve 0 o un código negativo de rsxkit. */
int salsa64_rdp_init(uint32_t scale);
void salsa64_rdp_shutdown(void);

/* Las dos manos de un backend gráfico (rsp/gfx/backend.h). */
const struct salsa64_gfx_sink *salsa64_rdp_sink(void);

/* Si el VI muestra una color image dibujada por el RSX en los últimos
 * campos, la presenta (en el cuadro en curso de rsxkit) y devuelve true. */
bool salsa64_rdp_present(const struct salsa64_vi_frame *f, uint32_t field, const rsxkit_view *view,
			 rsxkit_rect *out);

void salsa64_rdp_get_stats(struct salsa64_rdp_stats *out);

/* Escribe la salida de la última tarea (gfx.h: gfx_dump_header) en `path`,
 * con las texturas de la caché copiadas al arena (el volcado no depende de
 * la caché). Espera al RSX. Para depurar. */
bool salsa64_rdp_dump(const char *path);

#endif
