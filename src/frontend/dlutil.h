/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lo que no depende de la red en las descargas de carátulas y
 * trucos: el archivo descargas.cfg, las direcciones y el índice (la lista de
 * archivos que tiene el servidor). Sin dependencias de PSL1GHT: pruebas en
 * tests/unit/test_dlutil.c. La red está en download.c.
 */
#ifndef SALSA64_FRONTEND_DLUTIL_H
#define SALSA64_FRONTEND_DLUTIL_H

#include <stdbool.h>
#include <stddef.h>

#define DL_URL_MAX 400

/* descargas.cfg: de dónde se baja cada cosa. Las dos direcciones base
 * terminan en «/»; el índice es una dirección que devuelve la lista de
 * archivos (una por línea o la respuesta JSON de la API de GitHub). Sin
 * índice se usa «<base>index.txt». */
struct dl_cfg {
	char covers[DL_URL_MAX], covers_index[DL_URL_MAX];
	char cheats[DL_URL_MAX], cheats_index[DL_URL_MAX];
};

void dl_cfg_default(struct dl_cfg *c);
/* «clave = dirección» por línea; # comenta. Lo que no entiende lo deja. */
void dl_cfg_parse(struct dl_cfg *c, const char *text, size_t len);
/* El texto con que se crea descargas.cfg la primera vez (con comentarios). */
const char *dl_cfg_template(void);

/* Escapa un nombre de archivo para ponerlo en una dirección (%20, %28...). */
void dl_url_escape(const char *name, char *out, size_t size);

/* Del texto del índice saca los nombres que terminan en `ext`, uno por línea,
 * en `out` (termina en '\0'; devuelve su largo). Acepta una lista de texto o
 * JSON (los valores de «name» y el último tramo de «path»). Lo que no cabe se
 * descarta. */
size_t dl_index_names(const char *text, size_t len, const char *ext, char *out, size_t size);

#endif
