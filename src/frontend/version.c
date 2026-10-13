/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — versión, título y APPID del programa. Ver version.h.
 */
#include "version.h"

#include <stdio.h>
#include <string.h>

/* Los define src/frontend/Makefile solo para este archivo; las pruebas del
 * host lo compilan sin ellos. */
#ifndef SALSA64_VERSION
#define SALSA64_VERSION "desconocida"
#endif
#ifndef SALSA64_COMMIT
#define SALSA64_COMMIT ""
#endif
#ifndef SALSA64_TITLE
#define SALSA64_TITLE VERSION_TITLE_RELEASE
#endif
#ifndef SALSA64_APPID
#define SALSA64_APPID VERSION_APPID_RELEASE
#endif

const char salsa64_version[] = SALSA64_VERSION;
const char salsa64_commit[] = SALSA64_COMMIT;
const char salsa64_title[] = SALSA64_TITLE;
const char salsa64_appid[] = SALSA64_APPID;

static bool all_of(const char *s, size_t n, const char *set)
{
	if (!n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!s[i] || !strchr(set, s[i]))
			return false;
	return true;
}

#define HEX    "0123456789abcdef"
#define DIGITS "0123456789"

void version_label(const char *describe, const char *commit, char *buf, size_t size)
{
	size_t len = strlen(describe);
	const char *dirty = "";
	if (len >= 6 && !strcmp(describe + len - 6, "-dirty"))
		dirty = "-dirty", len -= 6;
	if (!len || (len == 11 && !memcmp(describe, "desconocida", 11))) {
		snprintf(buf, size, "versión desconocida");
		return;
	}

	/* «<etiqueta>-<n>-g<commit>»: se lee desde el final (la etiqueta puede
	 * tener guiones). */
	for (size_t i = len - 1; i >= 2; i--) {
		if (describe[i - 1] != '-' || describe[i] != 'g')
			continue;
		const char *h = describe + i + 1, *g = describe + i - 1, *d = g;
		while (d > describe && d[-1] != '-')
			d--;
		if (all_of(h, describe + len - h, HEX) && d - describe >= 2 && all_of(d, (size_t)(g - d), DIGITS)) {
			snprintf(buf, size, "%.*s+%.*s · %.*s%s", (int)(d - 1 - describe), describe, (int)(g - d), d,
				 (int)(describe + len - h), h, dirty);
			return;
		}
		break;
	}

	/* Sin etiquetas, git describe --always da el commit abreviado. */
	size_t cl = strlen(commit);
	if (all_of(describe, len, HEX) && len >= 7 && (!cl || !strncmp(describe, commit, len < cl ? len : cl))) {
		snprintf(buf, size, "%.*s%s", (int)len, describe, dirty);
		return;
	}
	/* Justo en una etiqueta. */
	if (cl)
		snprintf(buf, size, "%.*s · %s%s", (int)len, describe, commit, dirty);
	else
		snprintf(buf, size, "%.*s%s", (int)len, describe, dirty);
}

bool version_build(const char *title, const char *appid, char *buf, size_t size)
{
	if (!strcmp(title, VERSION_TITLE_RELEASE) && !strcmp(appid, VERSION_APPID_RELEASE))
		return false;
	snprintf(buf, size, "%s · %s", title, appid);
	return true;
}

void version_banner(char *buf, size_t size)
{
	snprintf(buf, size, "Salsa64 %s (%s, %s)",
		 strcmp(salsa64_version, "desconocida") ? salsa64_version : "versión desconocida", salsa64_title,
		 salsa64_appid);
}
