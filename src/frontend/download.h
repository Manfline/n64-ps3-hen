/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — descarga de carátulas y trucos desde el menú (PS3: libhttp).
 *
 * Un hilo aparte, para que el menú siga dibujando y se pueda cancelar:
 *  1. lee descargas.cfg (direcciones; si no existe lo crea con las de
 *     GitHub, ver dlutil.h);
 *  2. baja el índice (la lista de archivos del servidor);
 *  3. de cada ROM de la lista que aún no tiene carátula (o truco) busca en el
 *     índice, con el mismo criterio de nombres parecidos y de región que la
 *     búsqueda local (namematch.h), y baja ese archivo a covers/ (o cheats/).
 * Ver también docs de LEEME-extras.md sobre HTTPS.
 */
#ifndef SALSA64_FRONTEND_DOWNLOAD_H
#define SALSA64_FRONTEND_DOWNLOAD_H

#include <stdbool.h>

#define DL_NAME_MAX 128

/* Lo que la búsqueda de nombres necesita de cada ROM. */
struct dl_rom {
	char name[DL_NAME_MAX];  /* archivo, sin carpeta */
	char title[40];
	char code[5];
};

enum dl_state { DL_IDLE, DL_RUNNING, DL_DONE, DL_FAILED, DL_CANCELLED };

struct dl_status {
	int state;                         /* enum dl_state */
	int what;                          /* 0 carátulas, 1 trucos */
	unsigned done, total;              /* ROMs revisadas / ROMs */
	unsigned got, have, missing;       /* bajadas, ya había, sin coincidencia */
	char err[96];                      /* con DL_FAILED, el motivo (ya traducido) */
};

/* Empieza una descarga de lo que falta a las `n` ROMs (se copian). false si
 * ya hay una en curso o no se pudo crear el hilo. */
bool dl_start(int what, const struct dl_rom *roms, unsigned n);
void dl_cancel(void);
void dl_get(struct dl_status *st);
/* Al cerrar el programa: cancela y espera al hilo. */
void dl_shutdown(void);

#endif
