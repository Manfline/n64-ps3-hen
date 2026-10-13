/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — idiomas. tr("texto en español") da el texto en el idioma elegido
 * (Opciones > Idioma; menu.cfg «idioma = es|en|fr|de|it|pt»). Lo que no está
 * en la tabla (i18n.c) sale tal cual. Sin dependencias de PSL1GHT.
 */
#ifndef SALSA64_FRONTEND_I18N_H
#define SALSA64_FRONTEND_I18N_H

#include <stddef.h>

/* Idiomas con tabla (todos menos el español, que es la clave). */
#define MENU_LANGS_TR 5

/* 0 español, 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 portugués
 * (los valores de enum menu_lang en menu.h). */
void i18n_set(int lang);
int i18n_lang(void);
const char *tr(const char *es);

/* Para las pruebas: la tabla entera. */
size_t i18n_count(void);
const char *i18n_key(size_t i);
const char *i18n_text(size_t i, int lang);

#endif
