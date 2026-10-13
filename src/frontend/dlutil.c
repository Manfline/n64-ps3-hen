/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — descargas: configuración, direcciones e índice; ver dlutil.h. */
#include "dlutil.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define COVERS_BASE \
	"https://raw.githubusercontent.com/HarryTheTurtle/RetroArch-Nintendo-64-USA-Box-Art/master/Nintendo%20-%20Nintendo%2064/Named_Boxarts/"
#define COVERS_INDEX \
	"https://api.github.com/repos/HarryTheTurtle/RetroArch-Nintendo-64-USA-Box-Art/contents/Nintendo%20-%20Nintendo%2064/Named_Boxarts"
#define CHEATS_BASE "https://raw.githubusercontent.com/project64/project64/develop/Config/Cheats/"
#define CHEATS_INDEX "https://api.github.com/repos/project64/project64/contents/Config/Cheats?ref=develop"

void dl_cfg_default(struct dl_cfg *c)
{
	c->enabled = false;
	snprintf(c->covers, sizeof c->covers, "%s", COVERS_BASE);
	snprintf(c->covers_index, sizeof c->covers_index, "%s", COVERS_INDEX);
	snprintf(c->cheats, sizeof c->cheats, "%s", CHEATS_BASE);
	snprintf(c->cheats_index, sizeof c->cheats_index, "%s", CHEATS_INDEX);
}

const char *dl_cfg_template(void)
{
	return "# Salsa64: descargas de carátulas y trucos (Opciones > Descargar). Experimental:\n"
	       "# sin «activar = si» el menú no las ofrece. Los certificados HTTPS no se verifican.\n"
	       "activar = si\n"
	       "# Cada cosa tiene una dirección base (donde están los archivos) y un índice\n"
	       "# (una dirección que devuelve la lista de archivos: un nombre por línea, o la\n"
	       "# respuesta JSON de la API de GitHub). Si no pones índice se usa <base>index.txt.\n"
	       "# Si la consola no consigue conectar por HTTPS (GitHub exige TLS moderno), pon\n"
	       "# aquí un servidor HTTP de tu red, p. ej. http://IP-DE-TU-PC:8000/covers/\n"
	       "caratulas = " COVERS_BASE "\n"
	       "caratulas_indice = " COVERS_INDEX "\n"
	       "trucos = " CHEATS_BASE "\n"
	       "trucos_indice = " CHEATS_INDEX "\n";
}

static void set_url(char *dst, const char *val, bool base)
{
	size_t n = strlen(val);
	if (!n || n + 2 >= DL_URL_MAX)
		return;
	memcpy(dst, val, n);
	if (base && dst[n - 1] != '/')
		dst[n++] = '/';
	dst[n] = '\0';
}

void dl_cfg_parse(struct dl_cfg *c, const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[DL_URL_MAX + 40];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
			line[--n] = '\0';
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || !*p)
			continue;
		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = p, *val = eq + 1;
		for (char *k = key + strlen(key); k > key && (k[-1] == ' ' || k[-1] == '\t'); k--)
			k[-1] = '\0';
		while (*val == ' ' || *val == '\t')
			val++;
		if (!strcasecmp(key, "activar"))
			c->enabled = !strcasecmp(val, "si") || !strcasecmp(val, "sí") || !strcmp(val, "1");
		else if (!strcasecmp(key, "caratulas"))
			set_url(c->covers, val, true);
		else if (!strcasecmp(key, "caratulas_indice"))
			set_url(c->covers_index, val, false);
		else if (!strcasecmp(key, "trucos"))
			set_url(c->cheats, val, true);
		else if (!strcasecmp(key, "trucos_indice"))
			set_url(c->cheats_index, val, false);
	}
}

void dl_url_escape(const char *name, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p && o + 4 < size; p++) {
		unsigned char c = *p;
		bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
			     c == '.' || c == '_' || c == '~';
		if (plain) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	if (size)
		out[o < size ? o : size - 1] = '\0';
}

/* ---- Índice ---- */

static size_t add_name(char *out, size_t size, size_t used, const char *name, size_t n, const char *ext)
{
	size_t el = strlen(ext);
	/* Solo el último tramo (la API de árboles da la ruta entera). */
	for (size_t i = 0; i < n; i++)
		if (name[i] == '/') {
			name += i + 1;
			n -= i + 1;
			i = (size_t)-1;
		}
	if (n <= el || strncasecmp(name + n - el, ext, el) || used + n + 2 > size)
		return used;
	memcpy(out + used, name, n);
	out[used + n] = '\n';
	return used + n + 1;
}

static int hexv(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* La cadena JSON que empieza tras la comilla en text[*i]; la deja decodificada
 * en `buf` y avanza *i más allá de la comilla final. */
static size_t json_string(const char *text, size_t len, size_t *i, char *buf, size_t size)
{
	size_t n = 0;
	while (*i < len && text[*i] != '"') {
		char c = text[(*i)++];
		if (c == '\\' && *i < len) {
			char d = text[(*i)++];
			if (d == 'u' && *i + 4 <= len) {
				int v = 0, ok = 1;
				for (int k = 0; k < 4; k++) {
					int h = hexv(text[*i + k]);
					ok &= h >= 0;
					v = v * 16 + (h < 0 ? 0 : h);
				}
				*i += 4;
				if (!ok)
					v = '?';
				if (v < 0x80) {
					if (n + 1 < size)
						buf[n++] = (char)v;
				} else if (v < 0x800) {
					if (n + 2 < size) {
						buf[n++] = (char)(0xC0 | v >> 6);
						buf[n++] = (char)(0x80 | (v & 63));
					}
				} else if (n + 3 < size) {
					buf[n++] = (char)(0xE0 | v >> 12);
					buf[n++] = (char)(0x80 | (v >> 6 & 63));
					buf[n++] = (char)(0x80 | (v & 63));
				}
				continue;
			}
			c = d == 'n' ? '\n' : d == 't' ? '\t' : d;
		}
		if (n + 1 < size)
			buf[n++] = c;
	}
	if (*i < len)
		(*i)++;
	buf[n] = '\0';
	return n;
}

size_t dl_index_names(const char *text, size_t len, const char *ext, char *out, size_t size)
{
	size_t used = 0, s = 0;
	if (!size)
		return 0;
	while (s < len && (text[s] == ' ' || text[s] == '\n' || text[s] == '\r' || text[s] == '\t' || (unsigned char)text[s] == 0xEF ||
			   (unsigned char)text[s] == 0xBB || (unsigned char)text[s] == 0xBF))
		s++;
	if (s < len && (text[s] == '[' || text[s] == '{')) {
		/* JSON: cada «"name": "..."» o «"path": "..."». */
		char key[16], val[300];
		size_t i = s;
		while (i < len) {
			if (text[i] != '"') {
				i++;
				continue;
			}
			i++;
			size_t kn = json_string(text, len, &i, key, sizeof key);
			(void)kn;
			size_t j = i;
			while (j < len && (text[j] == ' ' || text[j] == '\n' || text[j] == '\r' || text[j] == '\t'))
				j++;
			if (j >= len || text[j] != ':')
				continue;  /* era un valor, no una clave */
			j++;
			while (j < len && (text[j] == ' ' || text[j] == '\n' || text[j] == '\r' || text[j] == '\t'))
				j++;
			if (j >= len || text[j] != '"') {
				i = j;
				continue;
			}
			i = j + 1;
			size_t vn = json_string(text, len, &i, val, sizeof val);
			if (!strcmp(key, "name") || !strcmp(key, "path"))
				used = add_name(out, size, used, val, vn, ext);
		}
	} else {
		size_t i = s;
		while (i < len) {
			size_t e = i;
			while (e < len && text[e] != '\n')
				e++;
			size_t n = e - i;
			while (n && (text[i + n - 1] == '\r' || text[i + n - 1] == ' '))
				n--;
			used = add_name(out, size, used, text + i, n, ext);
			i = e + 1;
		}
	}
	out[used] = '\0';
	return used;
}
