/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — búsqueda de archivos por nombre parecido; ver namematch.h. */
#include "namematch.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { R_US = 1, R_EU = 2, R_JP = 4, R_ALL = 7 };

/* Clave: minúsculas y solo letras y cifras, sin lo que va entre () o []. */
static size_t name_key(const char *s, size_t n, char *key, size_t size)
{
	size_t k = 0;
	int depth = 0;
	for (size_t i = 0; i < n && s[i]; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '(' || ch == '[')
			depth++;
		else if (ch == ')' || ch == ']')
			depth = depth ? depth - 1 : 0;
		else if (!depth && isalnum(ch) && k + 1 < size)
			key[k++] = (char)tolower(ch);
	}
	key[k] = '\0';
	return k;
}

static bool has_word(const char *s, size_t n, const char *w)
{
	size_t wl = strlen(w);
	for (size_t i = 0; i + wl <= n; i++)
		if (!strncasecmp(s + i, w, wl))
			return true;
	return false;
}

/* Regiones que dicen las etiquetas (...) de un nombre: (U), (E), (JU), (USA),
 * (Europe), (World)...; 0 si no dice. */
static unsigned tag_regions(const char *s, size_t n)
{
	unsigned m = 0;
	for (size_t i = 0; i < n && s[i]; i++) {
		if (s[i] != '(')
			continue;
		size_t j = i + 1;
		while (j < n && s[j] && s[j] != ')')
			j++;
		size_t len = j - i - 1;
		const char *g = s + i + 1;
		bool letters = len > 0 && len <= 3;
		for (size_t k = 0; k < len && letters; k++)
			letters = strchr("UEJuej", g[k]) != NULL;
		if (letters) {
			for (size_t k = 0; k < len; k++) {
				char c = (char)toupper((unsigned char)g[k]);
				m |= c == 'U' ? R_US : c == 'E' ? R_EU : R_JP;
			}
		} else {
			if (has_word(g, len, "usa") || has_word(g, len, "canada"))
				m |= R_US;
			if (has_word(g, len, "europe") || has_word(g, len, "germany") || has_word(g, len, "france") ||
			    has_word(g, len, "spain") || has_word(g, len, "italy") || has_word(g, len, "australia") ||
			    has_word(g, len, "uk"))
				m |= R_EU;
			if (has_word(g, len, "japan"))
				m |= R_JP;
			if (has_word(g, len, "world"))
				m |= R_ALL;
		}
		i = j;
	}
	return m;
}

/* Región por la letra 4 del código de la ROM (NUS-NXXE...). */
static unsigned code_regions(const char *code)
{
	if (!code || strlen(code) < 4)
		return 0;
	switch (code[3]) {
	case 'E':
		return R_US;
	case 'J':
		return R_JP;
	case 'P': case 'D': case 'F': case 'S': case 'I': case 'X': case 'Y': case 'L': case 'U': case 'H':
		return R_EU;
	default:
		return 0;
	}
}

/* Qué tan parecida es la clave del archivo (`file`) a la de la ROM (`want`),
 * de 0 (nada) a 4 (igual). Los títulos de la cabecera son cortos y sin
 * espacios («MARIOGOLF64», «MACE»): vale que uno esté dentro del otro. */
static int fuzzy(const char *file, size_t fn, const char *want, size_t wn, size_t min_key)
{
	if (wn < 3 || !fn)
		return 0;
	if (!strcmp(file, want))
		return 4;
	if (wn < min_key)
		return 0;
	if (strstr(file, want) && (wn >= 5 || !strncmp(file, want, wn)))
		return 3;  /* «indianajones» en «indianajonesandtheinfernalmachine» */
	if (fn >= 5 && fn * 10 >= wn * 6 && strstr(want, file))
		return 2;  /* «mariogolf» en «mariogolf64» */
	return 0;
}

/* De dónde salen los nombres: una carpeta o una lista con un nombre por línea. */
struct name_src {
	DIR *dir;
	const char *list;
	size_t len, pos;
};

static bool src_next(struct name_src *src, char *name, size_t size)
{
	if (src->dir) {
		struct dirent *de = readdir(src->dir);
		if (!de)
			return false;
		size_t n = strnlen(de->d_name, size - 1);
		memcpy(name, de->d_name, n);
		name[n] = '\0';
		return true;
	}
	while (src->pos < src->len) {
		size_t e = src->pos;
		while (e < src->len && src->list[e] != '\n')
			e++;
		size_t n = e - src->pos;
		while (n && (src->list[src->pos + n - 1] == '\r' || src->list[src->pos + n - 1] == ' '))
			n--;
		if (n >= size)
			n = size - 1;
		memcpy(name, src->list + src->pos, n);
		name[n] = '\0';
		src->pos = e + 1;
		if (n)
			return true;
	}
	return false;
}

static bool name_search(struct name_src *src, const char *ext, const char *rom_name, const char *title, const char *code,
			size_t min_key, char *found, size_t size)
{
	const char *dot = strrchr(rom_name, '.');
	size_t stem = dot && dot > rom_name ? (size_t)(dot - rom_name) : strlen(rom_name);
	char kr[128], kt[128];
	size_t nr = name_key(rom_name, stem, kr, sizeof kr);
	size_t nt = title ? name_key(title, strlen(title), kt, sizeof kt) : 0;
	if (!nr && !nt)
		return false;
	unsigned want = tag_regions(rom_name, stem);
	if (!want)
		want = code_regions(code);
	size_t el = strlen(ext);
	int best = 0, tier = 0;
	bool ambiguous = false;
	char tier_key[128] = "", name[256];
	while (src_next(src, name, sizeof name)) {
		size_t n = strlen(name);
		if (n <= el || strcasecmp(name + n - el, ext))
			continue;
		char kc[128];
		if (!name_key(name, n - el, kc, sizeof kc))
			continue;
		size_t kn = strlen(kc);
		int s = fuzzy(kc, kn, kr, nr, min_key), s2 = fuzzy(kc, kn, kt, nt, min_key);
		if (s2 > s)
			s = s2;
		if (!s)
			continue;
		unsigned have = tag_regions(name, n - el);
		if (want && have && !(want & have))
			continue;  /* otra región */
		/* Un parecido que no es igual y que cuadra con juegos distintos («mario»:
		 * Kart, Golf, Party...) no es de ninguno. */
		if (s > tier) {
			tier = s;
			ambiguous = false;
			snprintf(tier_key, sizeof tier_key, "%s", kc);
		} else if (s == tier && strcmp(tier_key, kc)) {
			ambiguous = true;
		}
		/* Mejor parecido, luego la misma región, luego el nombre más corto. */
		int score = s * 1000 + (want && have ? 500 : 0) - (int)(kn > 400 ? 400 : kn);
		if (score > best) {
			best = score;
			snprintf(found, size, "%s", name);
		}
	}
	return best > 0 && !(tier < 4 && ambiguous);
}

bool name_find(const char *dir, const char *ext, const char *rom_name, const char *title, const char *code, size_t min_key,
	       char *path, size_t size)
{
	struct name_src src = { opendir(dir), NULL, 0, 0 };
	if (!src.dir)
		return false;
	char found[256];
	bool ok = name_search(&src, ext, rom_name, title, code, min_key, found, sizeof found);
	closedir(src.dir);
	if (ok)
		snprintf(path, size, "%s/%s", dir, found);
	return ok;
}

bool name_find_list(const char *list, size_t len, const char *ext, const char *rom_name, const char *title,
		    const char *code, size_t min_key, char *name, size_t size)
{
	struct name_src src = { NULL, list, len, 0 };
	return name_search(&src, ext, rom_name, title, code, min_key, name, size);
}
