/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — trucos (códigos GameShark). Ver cheats.h.
 */
#include "cheats.h"
#include "namematch.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/bus.h"
#include "core/endian.h"

#define CHT_MAX_BYTES (1024u * 1024u)
#define ON_MAX_BYTES  (128u * 1024u)

/* ---- Lectura del .cht --------------------------------------------------------- */

static bool supported_type(unsigned t)
{
	switch (t) {
	case 0x80: case 0x81: case 0xA0: case 0xA1: case 0xF0: case 0xF1:
	case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0x50:
		return true;
	default:
		return false;
	}
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Un número hexadecimal de `min` a `max` dígitos; avanza *p. */
static bool hex_token(const char **p, unsigned min, unsigned max, uint32_t *out)
{
	const char *s = *p;
	uint32_t v = 0;
	unsigned n = 0;
	while (n < max && hexval((unsigned char)*s) >= 0) {
		v = (v << 4) | (uint32_t)hexval((unsigned char)*s);
		s++;
		n++;
	}
	if (n < min || (hexval((unsigned char)*s) >= 0))
		return false;  /* corto, o con más dígitos de los que caben */
	*out = v;
	*p = s;
	return true;
}

/* Las líneas de un valor «XXXXXXXX YYYY+XXXXXXXX YYYY». Una línea mal
 * formada (o con «?») se salta y se cuenta. Devuelve cuántas guardó. */
static unsigned parse_code(struct cheat_set *cs, const char *s, const char *end)
{
	unsigned got = 0;
	while (s < end) {
		while (s < end && (isspace((unsigned char)*s) || *s == '+' || *s == '"'))
			s++;
		if (s >= end)
			break;
		const char *piece = s;
		uint32_t a, v;
		bool ok = hex_token(&s, 8, 8, &a);
		while (ok && s < end && (*s == ' ' || *s == '\t'))
			s++;
		ok = ok && hex_token(&s, 1, 8, &v);
		if (!ok) {
			/* Hasta el próximo «+» (o el final). */
			s = piece;
			while (s < end && *s != '+')
				s++;
			cs->unsupported++;
			continue;
		}
		if (cs->nlines >= CHEATS_LINES_MAX) {
			cs->dropped++;
			continue;
		}
		if (!supported_type(a >> 24))
			cs->unsupported++;
		cs->l[cs->nlines].addr = a;
		cs->l[cs->nlines].val = v;
		cs->nlines++;
		got++;
	}
	return got;
}

static void copy_desc(char *dst, const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t' || *s == '"'))
		s++;
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '"' || end[-1] == '\r'))
		end--;
	size_t n = (size_t)(end - s);
	if (n > CHEAT_DESC_MAX - 1) {
		n = CHEAT_DESC_MAX - 1;
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
			n--;  /* no cortar un carácter UTF-8 a la mitad */
	}
	memcpy(dst, s, n);
	dst[n] = '\0';
}

void cheats_parse(struct cheat_set *cs, const char *text, size_t len)
{
	/* Por índice del archivo: el orden de las líneas no importa. */
	static struct cheat tmp[CHEATS_MAX];
	static bool seen[CHEATS_MAX];
	memset(cs, 0, sizeof *cs);
	memset(tmp, 0, sizeof tmp);
	memset(seen, 0, sizeof seen);
	unsigned top = 0;

	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		const char *line = text + i, *lend = text + e;
		i = e + 1;
		while (line < lend && (*line == ' ' || *line == '\t'))
			line++;
		if (lend - line < 8 || strncasecmp(line, "cheat", 5) != 0 || !isdigit((unsigned char)line[5]))
			continue;  /* «cheats = N», comentarios, vacías */
		const char *p = line + 5;
		unsigned idx = 0;
		while (p < lend && isdigit((unsigned char)*p)) {
			if (idx < 100000)
				idx = idx * 10 + (unsigned)(*p - '0');
			p++;
		}
		if (p >= lend || *p != '_')
			continue;
		p++;
		const char *eq = p;
		while (eq < lend && *eq != '=')
			eq++;
		if (eq >= lend)
			continue;
		const char *kend = eq;
		while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t'))
			kend--;
		size_t klen = (size_t)(kend - p);
		const char *val = eq + 1;
		while (val < lend && (*val == ' ' || *val == '\t'))
			val++;
		if (idx >= CHEATS_MAX) {
			cs->dropped++;
			continue;
		}
		if (idx + 1 > top)
			top = idx + 1;
		seen[idx] = true;
		if (klen == 4 && !strncasecmp(p, "desc", 4)) {
			copy_desc(tmp[idx].desc, val, lend);
		} else if (klen == 4 && !strncasecmp(p, "code", 4)) {
			if (!tmp[idx].count) {
				tmp[idx].first = (uint16_t)cs->nlines;
				tmp[idx].count = (uint16_t)parse_code(cs, val, lend);
			}
		} else if (klen == 6 && !strncasecmp(p, "enable", 6)) {
			tmp[idx].on = !strncasecmp(val, "true", 4) || *val == '1';
		}
	}

	for (unsigned k = 0; k < top; k++) {
		if (!seen[k] || !tmp[k].count)
			continue;
		if (!tmp[k].desc[0])
			snprintf(tmp[k].desc, sizeof tmp[k].desc, "Truco %u", k + 1);
		cs->c[cs->n++] = tmp[k];
	}
}

/* ---- .cht de Project64 ------------------------------------------------------------ */

static bool is_pj64(const char *text, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if (text[i] == '$' && (i == 0 || text[i - 1] == '\n'))
			return true;
	return false;
}

struct pj {
	char name[CHEAT_DESC_MAX];
	unsigned first, count;
	unsigned nq, qi[8], qmask[8];  /* líneas con «??» y qué dígitos */
	unsigned nopt;
	struct {
		uint32_t v;
		char name[40];
	} opt[48];
};

/* `s` en CHEAT_DESC_MAX bytes: si no cabe, se corta por la izquierda. */
static void fit_tail(char *dst, const char *s)
{
	size_t n = strlen(s);
	if (n <= CHEAT_DESC_MAX - 1) {
		memcpy(dst, s, n + 1);
		return;
	}
	size_t cut = n - (CHEAT_DESC_MAX - 1) + 3;
	while (cut < n && ((unsigned char)s[cut] & 0xC0) == 0x80)
		cut++;  /* no partir un carácter UTF-8 */
	snprintf(dst, CHEAT_DESC_MAX, "...%s", s + cut);
}

/* «A\B\C» -> «A > B > C»; si no cabe, se corta por la izquierda (lo que
 * distingue a un truco de otro está al final). */
static void pj_name(char *dst, const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		end--;
	char buf[256];
	size_t n = 0;
	for (const char *p = s; p < end && n + 4 < sizeof buf; p++) {
		if (*p == '\\') {
			memcpy(buf + n, " > ", 3);
			n += 3;
		} else {
			buf[n++] = *p;
		}
	}
	buf[n] = '\0';
	fit_tail(dst, buf);
}

static bool pj_add(struct cheat_set *cs, const char *name, unsigned first, unsigned count)
{
	if (!count)
		return false;
	if (cs->n >= CHEATS_MAX) {
		cs->dropped++;
		return false;
	}
	struct cheat *c = &cs->c[cs->n++];
	memset(c, 0, sizeof *c);
	fit_tail(c->desc, name);
	c->first = (uint16_t)first;
	c->count = (uint16_t)count;
	return true;
}

static void pj_finish(struct cheat_set *cs, struct pj *p)
{
	if (p->count && !p->nq) {
		pj_add(cs, p->name, p->first, p->count);
	} else if (p->count) {
		/* Una copia de las líneas por opción (la primera opción usa las originales,
		 * y se hace al final para que las copias salgan de las líneas sin tocar). */
		unsigned n0 = cs->n;
		for (unsigned o = p->nopt; o-- > 0;) {
			unsigned first = p->first;
			if (o > 0) {
				if (cs->nlines + p->count > CHEATS_LINES_MAX) {
					cs->dropped++;
					continue;
				}
				first = cs->nlines;
				memcpy(&cs->l[first], &cs->l[p->first], p->count * sizeof cs->l[0]);
				cs->nlines += p->count;
			}
			for (unsigned q = 0; q < p->nq; q++) {
				unsigned m = p->qmask[q], sh = 0;
				while (m && !(m & 1)) {
					m >>= 1;
					sh++;
				}
				struct cheat_line *l = &cs->l[first + p->qi[q]];
				l->val |= (p->opt[o].v << sh) & p->qmask[q];
			}
			char nm[CHEAT_DESC_MAX * 2 + 8];
			snprintf(nm, sizeof nm, "%s: %s", p->name, p->opt[o].name);
			pj_add(cs, nm, first, p->count);
		}
		/* Se agregaron de la última opción a la primera: se vuelve al orden del archivo. */
		for (unsigned a = n0, b = cs->n; a + 1 < b; a++, b--) {
			struct cheat t = cs->c[a];
			cs->c[a] = cs->c[b - 1];
			cs->c[b - 1] = t;
		}
	}
	memset(p, 0, sizeof *p);
}

void cheats_parse_pj64(struct cheat_set *cs, const char *text, size_t len, uint32_t crc1, uint32_t crc2)
{
	memset(cs, 0, sizeof *cs);
	/* Secciones: cada «[...]» al principio de línea. Se elige la del CRC. */
	size_t sec_start = 0, sec_end = len, first_start = (size_t)-1, first_end = len;
	bool found = false, any = false;
	for (size_t i = 0; i < len;) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		if (text[i] == '[' && e - i >= 19) {
			const char *q = text + i + 1;
			uint32_t a = 0, b = 0;
			bool ok = hex_token(&q, 8, 8, &a) && *q == '-' && (q++, hex_token(&q, 8, 8, &b));
			if (ok) {
				if (found && sec_end == len)
					sec_end = i;
				if (any && first_end == len)
					first_end = i;
				if (!any) {
					first_start = e + 1;
					any = true;
				}
				if (!found && crc1 && a == crc1 && b == crc2) {
					sec_start = e + 1;
					found = true;
					sec_end = len;
				}
			}
		}
		i = e + 1;
	}
	if (!found) {
		if (any) {
			sec_start = first_start;
			sec_end = first_end;
			cs->crc_mismatch = crc1 != 0;
		} else {
			sec_start = 0;
			sec_end = len;
		}
	}
	if (sec_start > len)
		sec_start = len;

	static struct pj p;
	memset(&p, 0, sizeof p);
	for (size_t i = sec_start; i < sec_end;) {
		size_t e = i;
		while (e < sec_end && text[e] != '\n')
			e++;
		const char *line = text + i, *lend = text + e;
		i = e + 1;
		while (line < lend && (*line == ' ' || *line == '\t'))
			line++;
		while (lend > line && (lend[-1] == '\r' || lend[-1] == ' ' || lend[-1] == '\t'))
			lend--;
		if (line >= lend)
			continue;
		if (*line == '$') {
			pj_finish(cs, &p);
			pj_name(p.name, line + 1, lend);
			p.first = cs->nlines;
			continue;
		}
		if (!p.name[0])
			continue;
		const char *s = line;
		uint32_t a, v = 0, mask = 0;
		if (hex_token(&s, 8, 8, &a)) {
			while (s < lend && (*s == ' ' || *s == '\t'))
				s++;
			unsigned nd = 0;
			while (s < lend && nd < 8 && (hexval((unsigned char)*s) >= 0 || *s == '?')) {
				v <<= 4;
				mask <<= 4;
				if (*s == '?')
					mask |= 0xF;
				else
					v |= (uint32_t)hexval((unsigned char)*s);
				s++;
				nd++;
			}
			if (!nd || s != lend) {
				cs->unsupported++;
				continue;
			}
			if (cs->nlines >= CHEATS_LINES_MAX || (mask && p.nq >= 8)) {
				cs->dropped++;
				continue;
			}
			if (!supported_type(a >> 24))
				cs->unsupported++;
			if (mask) {
				p.qi[p.nq] = p.count;
				p.qmask[p.nq++] = mask;
			}
			cs->l[cs->nlines].addr = a;
			cs->l[cs->nlines].val = v;
			cs->nlines++;
			p.count++;
			continue;
		}
		/* «20 Nombre de la opción», solo después de un «??». */
		uint32_t ov;
		if (p.nq && hex_token(&s, 1, 4, &ov) && s < lend && *s == ' ' && p.nopt < 48) {
			while (s < lend && *s == ' ')
				s++;
			size_t n = (size_t)(lend - s);
			if (n >= sizeof p.opt[0].name)
				n = sizeof p.opt[0].name - 1;
			memcpy(p.opt[p.nopt].name, s, n);
			p.opt[p.nopt].name[n] = '\0';
			p.opt[p.nopt].v = ov;
			p.nopt++;
		}
	}
	pj_finish(cs, &p);
}

/* ---- Los activos ---------------------------------------------------------------- */

void cheats_sel_parse(struct cheat_set *cs, const char *text, size_t len)
{
	for (unsigned k = 0; k < cs->n; k++)
		cs->c[k].on = false;
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		size_t n = e - i;
		while (n && (text[i + n - 1] == '\r' || text[i + n - 1] == ' '))
			n--;
		if (n)
			for (unsigned k = 0; k < cs->n; k++)
				if (strlen(cs->c[k].desc) == n && !memcmp(cs->c[k].desc, text + i, n))
					cs->c[k].on = true;
		i = e + 1;
	}
}

int cheats_sel_format(const struct cheat_set *cs, char *buf, size_t size)
{
	size_t used = 0;
	if (size)
		buf[0] = '\0';
	for (unsigned k = 0; k < cs->n; k++) {
		if (!cs->c[k].on)
			continue;
		size_t n = strlen(cs->c[k].desc);
		if (used + n + 2 > size)
			return -1;
		memcpy(buf + used, cs->c[k].desc, n);
		buf[used + n] = '\n';
		used += n + 1;
		buf[used] = '\0';
	}
	return (int)used;
}

unsigned cheats_active(const struct cheat_set *cs)
{
	unsigned n = 0;
	for (unsigned k = 0; k < cs->n; k++)
		n += cs->c[k].on;
	return n;
}

/* ---- Archivos ------------------------------------------------------------------- */

static char *read_file(const char *path, size_t max, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = malloc(max + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	*len = fread(buf, 1, max, f);
	fclose(f);
	buf[*len] = '\0';
	return buf;
}

/* «<dir>/<stem>.cht» y «<dir>/<stem>.on» (stem = la ROM sin extensión). */
static void set_paths(struct cheat_set *cs, const char *dir, const char *stem, size_t stem_len)
{
	snprintf(cs->cht_path, sizeof cs->cht_path, "%s/%.*s.cht", dir, (int)stem_len, stem);
	snprintf(cs->on_path, sizeof cs->on_path, "%s/%.*s.on", dir, (int)stem_len, stem);
}

unsigned cheats_load(struct cheat_set *cs, const char *dir, const char *rom_name, const char *title, const char *code,
		     uint32_t crc1, uint32_t crc2)
{
	memset(cs, 0, sizeof *cs);
	const char *dot = strrchr(rom_name, '.');
	size_t stem_len = dot && dot > rom_name ? (size_t)(dot - rom_name) : strlen(rom_name);
	set_paths(cs, dir, rom_name, stem_len);

	size_t len = 0;
	char *text = read_file(cs->cht_path, CHT_MAX_BYTES, &len);
	if (!text) {
		/* Sin distinguir mayúsculas (la consola sí las distingue). */
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				size_t n = strlen(de->d_name);
				if (n == stem_len + 4 && !strncasecmp(de->d_name, rom_name, stem_len) &&
				    !strcasecmp(de->d_name + stem_len, ".cht")) {
					set_paths(cs, dir, de->d_name, stem_len);
					/* La extensión también como está en el disco. */
					snprintf(cs->cht_path, sizeof cs->cht_path, "%s/%s", dir, de->d_name);
					text = read_file(cs->cht_path, CHT_MAX_BYTES, &len);
					break;
				}
			}
			closedir(d);
		}
	}
	if (!text) {
		/* Nombre parecido y de la región de la ROM (los .cht de RetroArch). */
		char found[CHEATS_PATH_MAX];
		if (name_find(dir, ".cht", rom_name, title, code, 8, found, sizeof found)) {
			const char *base = strrchr(found, '/');
			base = base ? base + 1 : found;
			set_paths(cs, dir, base, strlen(base) - 4);
			snprintf(cs->cht_path, sizeof cs->cht_path, "%s", found);
			text = read_file(cs->cht_path, CHT_MAX_BYTES, &len);
		}
	}
	if (!text)
		return 0;
	char on_path[CHEATS_PATH_MAX], cht_path[CHEATS_PATH_MAX];
	memcpy(on_path, cs->on_path, sizeof on_path);
	memcpy(cht_path, cs->cht_path, sizeof cht_path);
	if (is_pj64(text, len))
		cheats_parse_pj64(cs, text, len, crc1, crc2);
	else
		cheats_parse(cs, text, len);
	free(text);
	memcpy(cs->on_path, on_path, sizeof on_path);
	memcpy(cs->cht_path, cht_path, sizeof cht_path);

	size_t olen = 0;
	char *on = read_file(cs->on_path, ON_MAX_BYTES, &olen);
	if (on) {
		cheats_sel_parse(cs, on, olen);
		free(on);
	}
	return cs->n;
}

bool cheats_save(const struct cheat_set *cs)
{
	if (!cs->on_path[0])
		return false;
	char *buf = malloc(ON_MAX_BYTES);
	if (!buf)
		return false;
	int n = cheats_sel_format(cs, buf, ON_MAX_BYTES);
	FILE *f = n >= 0 ? fopen(cs->on_path, "wb") : NULL;
	bool ok = f && fwrite(buf, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	free(buf);
	return ok;
}

/* ---- Aplicar a la RDRAM ----------------------------------------------------------- */

static bool in_ram(const struct salsa64_bus *bus, uint32_t a, uint32_t size)
{
	return a + size <= bus->rdram_size;
}

static void write_mem(struct salsa64_bus *bus, uint32_t a, uint32_t size, uint32_t v)
{
	if (!in_ram(bus, a, size))
		return;
	uint8_t *p = bus->rdram + a;
	if (size == 1) {
		if (salsa64_rd8(p) == (uint8_t)v)
			return;
		salsa64_wr8(p, (uint8_t)v);
	} else {
		if (salsa64_rd16(p) == (uint16_t)v)
			return;
		salsa64_wr16(p, (uint16_t)v);
	}
	salsa64_bus_rdram_written(bus, a, size);
}

static uint32_t read_mem(const struct salsa64_bus *bus, uint32_t a, uint32_t size)
{
	if (!in_ram(bus, a, size))
		return 0xFFFFFFFFu;  /* fuera de la RDRAM: nunca coincide con un valor de 8 o 16 bits */
	return size == 1 ? salsa64_rd8(bus->rdram + a) : salsa64_rd16(bus->rdram + a);
}

static void apply_cheat(const struct cheat_set *cs, const struct cheat *c, struct salsa64_bus *bus)
{
	unsigned skip = 0;
	for (unsigned i = 0; i < c->count; i++) {
		const struct cheat_line *ln = &cs->l[c->first + i];
		if (skip) {
			skip--;
			continue;
		}
		unsigned type = ln->addr >> 24;
		uint32_t a = ln->addr & 0x00FFFFFFu, v = ln->val;
		switch (type) {
		case 0x80: case 0xA0: case 0xF0:
			write_mem(bus, a, 1, v & 0xFF);
			break;
		case 0x81: case 0xA1: case 0xF1:
			write_mem(bus, a & ~1u, 2, v & 0xFFFF);
			break;
		case 0xD0:
			skip = read_mem(bus, a, 1) != (v & 0xFF);
			break;
		case 0xD1:
			skip = read_mem(bus, a & ~1u, 2) != (v & 0xFFFF);
			break;
		case 0xD2:
			skip = read_mem(bus, a, 1) == (v & 0xFF);
			break;
		case 0xD3:
			skip = read_mem(bus, a & ~1u, 2) == (v & 0xFFFF);
			break;
		case 0x50: {
			/* 50 00 nn ss  vvvv: repite la línea siguiente. */
			if (i + 1 >= c->count)
				return;
			const struct cheat_line *nx = &cs->l[c->first + i + 1];
			unsigned nt = nx->addr >> 24, count = (a >> 8) & 0xFF, step = a & 0xFF;
			uint32_t na = nx->addr & 0x00FFFFFFu, nv = nx->val;
			for (unsigned k = 0; k < count; k++) {
				uint32_t addr = na + k * step, val = nv + k * v;
				if (nt == 0x80 || nt == 0xA0 || nt == 0xF0)
					write_mem(bus, addr, 1, val & 0xFF);
				else if (nt == 0x81 || nt == 0xA1 || nt == 0xF1)
					write_mem(bus, addr & ~1u, 2, val & 0xFFFF);
			}
			i++;  /* la línea repetida ya se aplicó */
			break;
		}
		default:
			break;  /* no soportado: se ignora */
		}
	}
}

void cheats_apply(const struct cheat_set *cs, struct salsa64_bus *bus)
{
	for (unsigned k = 0; k < cs->n; k++)
		if (cs->c[k].on)
			apply_cheat(cs, &cs->c[k], bus);
}
