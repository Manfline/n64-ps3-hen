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


/* Los filtros CRT, originales del autor portados de sus versiones de
 * RetroMac (en el menú, los MENU_FILTER_CRT_* en este orden). */
enum salsa64_crt {
	SALSA64_CRT_NONE,
	SALSA64_CRT_MASK,       /* máscara de sombra (v1.3) */
	SALSA64_CRT_RGB,        /* CRT con cable (v1.1): euroconector RGB */
	SALSA64_CRT_SVIDEO,     /* ... S-Video */
	SALSA64_CRT_COMPOSITE,  /* ... compuesto */
	SALSA64_CRT_TRINITRON,  /* Trinitron de la sala (Sony KV-29), sin cable */
	SALSA64_CRTS
};

/* Nombre (log y avisos) y clave (la de menu.cfg y del archivo `filtro`). */
const char *salsa64_crt_name(int f);
const char *salsa64_crt_key(int f);
/* El filtro de una clave («crt-mascara»…), o -1. */
int salsa64_crt_from_key(const char *key);

struct salsa64_filter_src {
	rsxkit_texture tex;  /* la imagen: un destino a escala interna o el framebuffer 2D */
	rsxkit_rect region;  /* lo que se ve, en texeles */
	uint32_t factor;     /* texeles por píxel del juego (1 a 6) */
	float aspect;        /* forma de la imagen en la tele (4/3) */
	/* Con has_gen, un número que cambia cada vez que algo escribe en la
	 * imagen (salsa64_rdp_present_source): con el mismo, la misma textura y
	 * la misma región, el filtro no rehace sus tablas y solo dibuja su
	 * pasada final. */
	bool has_gen;
	uint32_t gen;
};

/* Dibuja la imagen con el filtro CRT `f` en la pantalla del cuadro en curso y
 * deja en `out` el rectángulo que ocupa. false si no se pudo (sin memoria
 * del RSX o un tamaño que no cabe): entonces hay que presentarla sin
 * filtro. La primera vez con cada tamaño espera al RSX para reservar sus
 * tablas. */
bool salsa64_filters_draw(int f, const struct salsa64_filter_src *src, rsxkit_rect *out);

/* Lo que tardaría (estimado, en ms) en preparar el filtro f para esta
 * imagen antes de dibujarla: tablas, programas y, la primera vez de cada
 * filtro, la comprobación en el RSX. 0 si ya está listo. Para avisar en
 * pantalla antes de un trabajo largo. */
uint32_t salsa64_filters_setup_ms(int f, const struct salsa64_filter_src *src);

/* Opciones de medición y diagnóstico. `captures`: la primera vez de cada
 * filtro y alto de pantalla, guarda la imagen que entra al filtro y la que
 * dibuja en SALSA64_DATA_DIR/filtro-<clave>-<alto>-fuente.ppm y
 * -pantalla.ppm (el RSX las copia a la memoria principal y otro hilo las
 * escribe). `reduce_passes`: rsxkit_post_reduce_passes (1 o 2). */
void salsa64_filters_options(bool captures, int reduce_passes);

/* La señal y la decodificación de S-Video y compuesto en dos SPEs en lugar
 * del RSX (crt_spe.h); vale desde el próximo cuadro. Si un SPE falla o no
 * termina a tiempo, vuelven al RSX por el resto de la corrida. */
void salsa64_filters_spe(bool on);

/* Líneas para el log (separadas por '\n') con lo que tardó en el RSX cada
 * pasada, en promedio por cuadro, desde el llamado anterior, y aparte el
 * SPE y los cuadros con la imagen repetida (vacía las cuentas). false si
 * en ese tiempo no se dibujó con filtro. */
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
