/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pruebas de los idiomas (src/frontend/i18n.c): la tabla va ordenada, cada
 * traducción tiene los mismos % que el original, solo usa caracteres que la
 * fuente del menú sabe dibujar y cabe donde cabía el español.
 */
#include <stdio.h>
#include <string.h>

#include <rsxkit/text.h>
#include "../../lib/rsxkit/src/font.h"

#include "frontend/i18n.h"
#include "test.h"

static void sorted(void)
{
	CHECK(i18n_count() > 100);
	for (size_t i = 1; i < i18n_count(); i++) {
		if (strcmp(i18n_key(i - 1), i18n_key(i)) >= 0) {
			printf("   fuera de orden o repetida: «%s» / «%s»\n", i18n_key(i - 1), i18n_key(i));
			CHECK(0);
		}
	}
}

/* Los % de un texto, en orden («%u», «%s», «%ux%u»...), sin los «%%». */
static void formats(const char *s, char *out, size_t size)
{
	size_t o = 0;
	for (; *s; s++) {
		if (*s != '%')
			continue;
		if (s[1] == '%') {
			s++;
			continue;
		}
		const char *e = s + 1;
		while (*e && strchr("-+ #0123456789.l", *e))
			e++;
		if (*e && o + (size_t)(e - s) + 2 < size) {
			memcpy(out + o, s, (size_t)(e - s) + 1);
			o += (size_t)(e - s) + 1;
			out[o++] = ';';
		}
		s = *e ? e : e - 1;
	}
	out[o] = '\0';
}

static void same_formats(void)
{
	for (size_t i = 0; i < i18n_count(); i++) {
		char want[64];
		formats(i18n_key(i), want, sizeof want);
		for (int l = 1; l <= MENU_LANGS_TR; l++) {
			char got[64];
			const char *t = i18n_text(i, l);
			CHECK(t != NULL && t[0]);
			formats(t ? t : "", got, sizeof got);
			if (strcmp(want, got)) {
				printf("   «%s» idioma %d: «%s» (%s / %s)\n", i18n_key(i), l, t, want, got);
				CHECK(0);
			}
		}
	}
}

static void glyphs(void)
{
	/* Los íconos de filas que el menú dibuja con la fuente. */
	const char *icons = "~□▶─A↓■▲●#?◀►";
	for (const char *p = icons; *p;) {
		uint32_t cp = rsxkit_utf8_next(&p);
		if (!rsxkit_font_has(cp)) {
			printf("   sin glifo U+%04X\n", cp);
			CHECK(0);
		}
	}
	for (size_t i = 0; i < i18n_count(); i++) {
		for (int l = 0; l <= MENU_LANGS_TR; l++) {
			const char *t = l ? i18n_text(i, l) : i18n_key(i);
			for (const char *p = t; *p;) {
				uint32_t cp = rsxkit_utf8_next(&p);
				if (cp >= 0x20 && !rsxkit_font_has(cp)) {
					printf("   «%s» idioma %d: sin glifo U+%04X\n", t, l, cp);
					CHECK(0);
				}
			}
		}
	}
}

/* Una traducción no debe ser mucho más larga que el español: donde cabía el
 * español (líneas de 30 celdas del panel, pie de 66) debe caber ella. */
static void widths(void)
{
	for (size_t i = 0; i < i18n_count(); i++) {
		uint32_t es = rsxkit_text_cells(i18n_key(i));
		for (int l = 1; l <= MENU_LANGS_TR; l++) {
			uint32_t n = rsxkit_text_cells(i18n_text(i, l));
			uint32_t limit = es <= 31 ? 34 : es <= 66 ? 66 : es + 8;
			if (n > limit) {
				printf("   «%s» idioma %d: %u celdas (límite %u)\n", i18n_key(i), l, n, limit);
				CHECK(0);
			}
		}
	}
}

/* Los rótulos de los datos de la ROM caben en su columna (9 celdas a 1,5×). */
static void labels(void)
{
	static const char *const keys[] = { "Archivo", "Ubicación", "Código", "Tamaño", "Región", "Guardado" };
	for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
		i18n_set(0);
		for (int l = 1; l <= MENU_LANGS_TR; l++) {
			i18n_set(l);
			uint32_t n = rsxkit_text_cells(tr(keys[k]));
			if (n > 9) {
				printf("   rótulo «%s» idioma %d: %u celdas\n", tr(keys[k]), l, n);
				CHECK(0);
			}
		}
	}
	i18n_set(0);
}

static void lookup(void)
{
	i18n_set(0);
	CHECK(!strcmp(tr("Volver"), "Volver"));
	i18n_set(1);
	CHECK(!strcmp(tr("Volver"), "Back"));
	CHECK(!strcmp(tr("Sí"), "Yes"));
	i18n_set(2);
	CHECK(!strcmp(tr("Volver"), "Retour"));
	i18n_set(3);
	CHECK(!strcmp(tr("Volver"), "Zurück"));
	i18n_set(4);
	CHECK(!strcmp(tr("Volver"), "Indietro"));
	i18n_set(5);
	CHECK(!strcmp(tr("Volver"), "Voltar"));
	/* Lo que no está sale igual (títulos de juegos, rutas...). */
	CHECK(!strcmp(tr("GoldenEye 007"), "GoldenEye 007"));
	CHECK(tr("Rareza") != NULL);
	i18n_set(9);  /* fuera de rango: español */
	CHECK_EQ(i18n_lang(), 0);
	CHECK(!strcmp(tr("Volver"), "Volver"));
	CHECK(tr(NULL) == NULL);
	i18n_set(0);
}

void t_suite_i18n(void)
{
	RUN(sorted);
	RUN(same_formats);
	RUN(glyphs);
	RUN(widths);
	RUN(labels);
	RUN(lookup);
}
