/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de filtros de la presentación. Ver catalog.h.
 */
#include "catalog.h"

#include <string.h>

static const struct {
	const char *name, *key;
} filters[SALSA64_FILTERS] = {
	[SALSA64_FILTER_NONE] = { "Ninguno", "ninguno" },
	[SALSA64_FILTER_CRT_MASK] = { "CRT máscara de sombra", "crt-mascara" },
	[SALSA64_FILTER_CRT_RGB] = { "CRT cable RGB", "crt-rgb" },
	[SALSA64_FILTER_CRT_SVIDEO] = { "CRT cable S-Video", "crt-svideo" },
	[SALSA64_FILTER_CRT_COMPOSITE] = { "CRT cable compuesto", "crt-compuesto" },
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
