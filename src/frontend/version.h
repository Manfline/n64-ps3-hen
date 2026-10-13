/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — de qué código y de qué paquete es el programa que corre. Varias
 * apps de prueba instaladas pueden llevar el mismo título en el XMB; esto
 * las distingue en el menú, en la pausa y en la primera línea del log.
 *
 * Lo pone src/frontend/Makefile al compilar version.c: `git describe --tags
 * --always --dirty` y el commit (tools/build.sh los calcula fuera del
 * contenedor, donde un worktree sí ve su .git), y el TITLE y el APPID con
 * que se arma el paquete. Sin git, la versión es «desconocida».
 */
#ifndef SALSA64_FRONTEND_VERSION_H
#define SALSA64_FRONTEND_VERSION_H

#include <stdbool.h>
#include <stddef.h>

/* Los del paquete publicado (src/frontend/Makefile). */
#define VERSION_TITLE_RELEASE "Salsa64"
#define VERSION_APPID_RELEASE "SLSA00064"

extern const char salsa64_version[];  /* «v0.2.5-3-gabc1234», «v0.2.5», «abc1234» (sin etiquetas)
                                         o «desconocida»; «-dirty» al final si había cambios sin commit */
extern const char salsa64_commit[];   /* «abc1234», o "" */
extern const char salsa64_title[];    /* TITLE del PARAM.SFO */
extern const char salsa64_appid[];    /* APPID */

/* La versión corta para la interfaz: «v0.2.5 · abc1234» en la etiqueta,
 * «v0.2.5+3 · abc1234» tres commits después, «abc1234» sin etiquetas, con
 * «-dirty» si lo dice `describe`; «versión desconocida» (el texto en
 * español: se traduce con tr()) sin git. Portable: tests/unit/test_menu.c. */
void version_label(const char *describe, const char *commit, char *buf, size_t size);

/* «título · APPID» si el paquete no es el publicado (un paquete de prueba),
 * o false sin escribir nada. */
bool version_build(const char *title, const char *appid, char *buf, size_t size);

/* La primera línea del log: «Salsa64 v0.2.5-3-gabc1234 (título, APPID)». */
void version_banner(char *buf, size_t size);

#endif
