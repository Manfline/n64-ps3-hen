/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de filtros de la presentación. Ver catalog.h.
 */
#include "catalog.h"

#include <string.h>

static const struct {
	const char *name, *key;
	const char *help[3];
} filters[SALSA64_FILTERS] = {
	[SALSA64_FILTER_NONE] = { "Ninguno", "ninguno",
				  { "La imagen tal cual, escalada", "con filtro lineal.", NULL } },
	[SALSA64_FILTER_CRT_MASK] = { "CRT máscara de sombra", "crt-mascara",
				      { "Monitor con puntos de fósforo", "redondos en triángulo.",
					"Filtro original del autor." } },
	[SALSA64_FILTER_CRT_RGB] = { "CRT cable RGB", "crt-rgb",
				     { "Tele Trinitron por euroconector", "RGB: imagen limpia.",
				       "Filtro original del autor." } },
	[SALSA64_FILTER_CRT_SVIDEO] = { "CRT cable S-Video", "crt-svideo",
					{ "Tele Trinitron por S-Video: el", "color se corre un poco.",
					  "Filtro original del autor." } },
	[SALSA64_FILTER_CRT_COMPOSITE] = { "CRT cable compuesto", "crt-compuesto",
					   { "Tele Trinitron por compuesto:", "sangrado de color y dot crawl.",
					     "Filtro original del autor." } },
};

const char *salsa64_filter_name(int f)
{
	return f >= 0 && f < SALSA64_FILTERS ? filters[f].name : "?";
}

const char *salsa64_filter_key(int f)
{
	return f >= 0 && f < SALSA64_FILTERS ? filters[f].key : "ninguno";
}

int salsa64_filter_from_key(const char *key, unsigned len)
{
	for (int f = 0; f < SALSA64_FILTERS; f++)
		if (strlen(filters[f].key) == len && !memcmp(filters[f].key, key, len))
			return f;
	return -1;
}

const char *const *salsa64_filter_help(int f)
{
	return filters[f >= 0 && f < SALSA64_FILTERS ? f : 0].help;
}
