/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de filtros de la presentación: nombre, clave (la del
 * archivo `filtro`) y descripción. No depende del RSX.
 *
 * Los filtros CRT son originales del autor, portados de sus versiones de
 * RetroMac.
 */
#ifndef SALSA64_FRONTEND_FILTERS_CATALOG_H
#define SALSA64_FRONTEND_FILTERS_CATALOG_H

enum salsa64_filter {
	SALSA64_FILTER_NONE,
	SALSA64_FILTER_CRT_MASK,       /* CRT de máscara de sombra (v1.3) */
	SALSA64_FILTER_CRT_RGB,        /* CRT con cable (v1.1): euroconector RGB */
	SALSA64_FILTER_CRT_SVIDEO,     /* ... S-Video */
	SALSA64_FILTER_CRT_COMPOSITE,  /* ... compuesto */
	SALSA64_FILTERS
};

/* Nombre para el log y los avisos. */
const char *salsa64_filter_name(int f);
/* Clave (archivo `filtro`). */
const char *salsa64_filter_key(int f);
/* El filtro de una clave, o -1. */
int salsa64_filter_from_key(const char *key, unsigned len);

#endif
