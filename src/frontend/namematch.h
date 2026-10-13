/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — buscar el archivo de un juego (carátula o trucos) por un nombre
 * parecido: los paquetes de RetroArch llevan nombres de No-Intro
 * («007 - GoldenEye (USA).png», «1080 Snowboarding (JU) (M2).cht») y las ROMs
 * del usuario casi nunca se llaman igual. Sin dependencias de PSL1GHT.
 */
#ifndef SALSA64_FRONTEND_NAMEMATCH_H
#define SALSA64_FRONTEND_NAMEMATCH_H

#include <stdbool.h>
#include <stddef.h>

/* Busca en `dir` un archivo con extensión `ext` («.png», sin distinguir
 * mayúsculas) cuyo nombre, sin lo que va entre () y [] ni signos, sea el de la
 * ROM o el título (o lo contenga, con 5 letras o más). Si el archivo y la ROM
 * dicen de qué región son (USA, Europa, Japón: por las etiquetas del nombre o,
 * en la ROM, por la letra de región de `code`) y no coinciden, no sirve: los
 * trucos de otra región escriben en otras direcciones. Entre los que sirven
 * gana el nombre igual, luego el que contiene o está dentro del otro, y a
 * igualdad la misma región. `min_key`: letras que debe tener el nombre de la
 * ROM para aceptar un parecido que no sea igual (los títulos cortos como «MACE»
 * o «RUSH» se confunden con otros juegos: 4 para carátulas, que si fallan solo
 * dan una imagen equivocada, y 8 para trucos, que escribirían en la RDRAM).
 * Deja la ruta en `path`. */
bool name_find(const char *dir, const char *ext, const char *rom_name, const char *title, const char *code, size_t min_key,
	       char *path, size_t size);

/* Lo mismo con una lista de nombres (uno por línea, p. ej. el índice de un
 * servidor) en lugar de una carpeta: deja el nombre elegido en `name`. */
bool name_find_list(const char *list, size_t len, const char *ext, const char *rom_name, const char *title,
		    const char *code, size_t min_key, char *name, size_t size);

#endif
