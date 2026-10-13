/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — filtros de la presentación en el RSX, sobre la maquinaria de
 * posprocesado de lib/rsxkit (rsxkit/post.h). Los filtros CRT son
 * originales del autor, portados de sus versiones de RetroMac; las cuentas
 * por imagen están en crt_params.c y las por píxel en los .fcg de este
 * directorio. Diseño, pasadas y costos medidos: docs/filtros.md.
 *
 * Cada filtro reduce primero la imagen del juego (dibujada a la escala
 * interna) a su resolución original, que es la que ve un CRT, y la dibuja
 * en la pantalla en el rectángulo de su forma (4:3).
 */
#ifndef SALSA64_FRONTEND_FILTERS_FILTERS_H
#define SALSA64_FRONTEND_FILTERS_FILTERS_H

#include <stdbool.h>
#include <stddef.h>

#include <rsxkit/rsxkit.h>

#include "frontend/filters/catalog.h"

struct salsa64_filter_src {
	rsxkit_texture tex;  /* la imagen: un destino a escala interna o el framebuffer 2D */
	rsxkit_rect region;  /* lo que se ve, en texeles */
	uint32_t factor;     /* texeles por píxel del juego (1 a 6) */
	float aspect;        /* forma de la imagen en la tele (4/3) */
};

/* Dibuja la imagen con el filtro `f` en la pantalla del cuadro en curso y
 * deja en `out` el rectángulo que ocupa. false si no se pudo (sin memoria
 * del RSX o un tamaño que no cabe): entonces hay que presentarla sin
 * filtro. La primera vez con cada tamaño espera al RSX para reservar sus
 * tablas. */
bool salsa64_filters_draw(int f, const struct salsa64_filter_src *src, rsxkit_rect *out);

/* Línea para el log con lo que tardó en el RSX cada pasada, en promedio
 * por cuadro, desde el llamado anterior (vacía las cuentas). false si en
 * ese tiempo no se dibujó con filtro. */
bool salsa64_filters_report(char *buf, size_t size);

/* Diagnóstico: después de cada cuadro con filtro, dibuja en la esquina de
 * la pantalla la tabla `name` («lineal», «resplandor», «puntos»,
 * «teselaA»…; NULL apaga), un texel por píxel, sus canales 0 a 2 en R, G y
 * B por `scale`. */
void salsa64_filters_debug(const char *name, float scale);

/* Suelta las tablas y los programas (al volver al menú o al cambiar la
 * salida de video): espera al RSX. */
void salsa64_filters_release(void);

#endif
